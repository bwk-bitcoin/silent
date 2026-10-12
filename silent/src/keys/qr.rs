//! The bwk-qr protocol codec: builds signing-flow requests as protocol messages, and
//! decodes and validates the matching responses. Framing the bytes is the transport's
//! business.
//!
//! `QrSession` owns the request id counter and remembers the one outstanding request,
//! so a decoded response is checked against what was actually asked before it is
//! accepted.

use std::str::FromStr;

use bitcoin::{
    bip32::{ChildNumber, DerivationPath, Fingerprint, Xpriv, Xpub},
    Network,
};
use bwk_qr_protocol::{
    request::{self, KeyKind},
    response,
    types::{self, SpKeys},
    Message, MessageType, Request, RequestId, Response, SignResponseKind,
};
use bwk_sp::{
    bwk_sign::bwk_descriptor::{
        descriptor::{tr_path, wpkh_path},
        sp_descriptor::SpDescriptor,
    },
    receiver::{scan_path, spend_path},
};

use crate::{
    config::sub_account,
    hwi,
    keys::{
        error::Error,
        hot::{sp_account, watch_only_sp_descriptor},
        psbt,
    },
};

/// What a `GetKeys` request asked for. Its response carries the keys in request
/// order and nothing else to pair them with.
enum KeysRequest {
    /// Every key public.
    Xpubs(Vec<DerivationPath>),
    SpAccount(SpAccountPaths),
}

/// The keys of a silent payment account: its spend key public and its scan key
/// private, then the taproot and segwit sub-accounts' xpubs.
struct SpAccountPaths {
    network: Network,
    account: ChildNumber,
    spend: DerivationPath,
    scan: DerivationPath,
    taproot: DerivationPath,
    segwit: DerivationPath,
}

impl KeysRequest {
    /// The keys asked for, in request order.
    fn keys(&self) -> Vec<(DerivationPath, KeyKind)> {
        match self {
            Self::Xpubs(paths) => paths
                .iter()
                .map(|path| (path.clone(), KeyKind::Public))
                .collect(),
            Self::SpAccount(paths) => vec![
                (paths.spend.clone(), KeyKind::Public),
                (paths.scan.clone(), KeyKind::Private),
                (paths.taproot.clone(), KeyKind::Public),
                (paths.segwit.clone(), KeyKind::Public),
            ],
        }
    }
}

/// What the next response has to match. `keys` is `None` for every request but
/// `GetKeys`. Read back by `validate_response` to check a decoded response against
/// what was actually asked.
struct Outstanding {
    id: RequestId,
    message_type: MessageType,
    keys: Option<KeysRequest>,
    /// The address a `VerifyAddress` request asked the device to confirm, so the
    /// response can be compared against it. `None` for every other request.
    expected_address: Option<String>,
    /// The original v2 PSBT bytes a `Sign` request carried, so `validate_signed` merges
    /// the device's answer into it instead of trusting the returned bytes as-is. `None`
    /// for every other request kind.
    original_psbt_v2: Option<Vec<u8>>,
}

/// Allocates request ids, builds the five signing-flow requests, and remembers the one
/// outstanding request a response is checked against.
#[derive(Default)]
pub struct QrSession {
    counter: u16,
    outstanding: Option<Outstanding>,
}

impl QrSession {
    pub fn new() -> Self {
        Self::default()
    }

    /// Clears any outstanding request and the id counter, for a network switch.
    pub fn reset(&mut self) {
        *self = Self::new();
    }

    pub fn outstanding_id(&self) -> Option<RequestId> {
        self.outstanding.as_ref().map(|outstanding| outstanding.id)
    }

    /// `RequestId([0, 0])` stays reserved for "no outstanding request", so the first
    /// id issued is 1.
    fn next_id(&mut self) -> RequestId {
        self.counter = self.counter.wrapping_add(1);
        RequestId(self.counter.to_be_bytes())
    }

    pub fn get_xpubs(&mut self, paths: Vec<DerivationPath>) -> Result<QrRequest, Error> {
        if paths.is_empty() {
            return Err(Error::EmptyPathList);
        }
        self.get_keys(KeysRequest::Xpubs(paths))
    }

    /// Asks for the silent payment account in one request: the signer gives out its
    /// scan key, never its spend key, and the sp() descriptor is built from them here.
    pub fn get_sp_account(&mut self, network: Network) -> Result<QrRequest, Error> {
        let account = sp_account()?;
        let sub_account = sub_account();
        let paths = SpAccountPaths {
            network,
            account,
            spend: spend_path(network, account),
            scan: scan_path(network, account),
            taproot: tr_path(network, sub_account).expect("the sub-account is hardened"),
            segwit: wpkh_path(network, sub_account).expect("the sub-account is hardened"),
        };
        self.get_keys(KeysRequest::SpAccount(paths))
    }

    fn get_keys(&mut self, keys: KeysRequest) -> Result<QrRequest, Error> {
        let requests = keys
            .keys()
            .iter()
            .map(|(path, kind)| request::KeyRequest {
                derivation_path: types::DerivationPath::from(path),
                kind: *kind,
            })
            .collect();
        let body = request::Body::GetKeys(request::GetKeys { keys: requests });
        self.issue(body, Some(keys), None, None)
    }

    pub fn register_descriptor(&mut self, descriptor: &str) -> Result<QrRequest, Error> {
        let body = request::Body::RegisterDescriptor(request::RegisterDescriptor {
            descriptor_alias: hwi::wallet_name(descriptor),
            descriptor: Some(request::DescriptorBody::Bip380(descriptor.to_string())),
        });
        self.issue(body, None, None, None)
    }

    /// A silent payment address is asked for with its account's keys instead of the
    /// sp() descriptor, which holds the scan secret: `path` is then its label, empty
    /// for the account's address.
    pub fn verify_address(
        &mut self,
        descriptor: &str,
        path: DerivationPath,
        address: String,
    ) -> Result<QrRequest, Error> {
        let body = match SpDescriptor::from_str(descriptor) {
            Ok(sp) => request::DescriptorBody::from(&sp_keys(&sp)?),
            Err(_) => request::DescriptorBody::Bip380(descriptor.to_string()),
        };
        let body = request::Body::VerifyAddress(request::VerifyAddress {
            descriptor_alias: hwi::wallet_name(descriptor),
            derivation_path: types::DerivationPath::from(&path),
            address: Some(address.clone()),
            descriptor: Some(body),
            proof: None,
        });
        self.issue(body, None, Some(address), None)
    }

    pub fn sign(
        &mut self,
        descriptors: &[String],
        psbt_v2_bytes: &[u8],
    ) -> Result<QrRequest, Error> {
        let descriptors = descriptors
            .iter()
            .map(|descriptor| request::Descriptor {
                alias: hwi::wallet_name(descriptor),
                descriptor: Some(request::DescriptorBody::Bip380(descriptor.clone())),
                proof: None,
            })
            .collect();
        let parsed = psbt::parse_v2(psbt_v2_bytes)?;
        // A silent-payment PSBT needs its input tweaks and ECDH shares, which a v0
        // conversion drops, so it goes over the wire unchanged. A plain hardware signer
        // over QR still expects v0.
        let payload = if psbt::has_sp_artifacts(&parsed)? {
            psbt_v2_bytes.to_vec()
        } else {
            parsed
                .into_bitcoin_psbt()
                .map_err(|e| Error::PsbtSerialize(e.to_string()))?
                .serialize()
        };
        let body = request::Body::Sign(request::Sign {
            descriptors,
            psbt: payload,
            want_kind: Some(SignResponseKind::Psbt),
        });
        self.issue(body, None, None, Some(psbt_v2_bytes.to_vec()))
    }

    /// Encodes the request and makes it the outstanding one.
    fn issue(
        &mut self,
        body: request::Body,
        keys: Option<KeysRequest>,
        expected_address: Option<String>,
        original_psbt_v2: Option<Vec<u8>>,
    ) -> Result<QrRequest, Error> {
        let id = self.next_id();
        let message_type = body.message_type();
        let message = bwk_qr_protocol::encode_request(&Request { id, body })?;
        self.outstanding = Some(Outstanding {
            id,
            message_type,
            keys,
            expected_address,
            original_psbt_v2,
        });
        Ok(QrRequest { id, message })
    }

    /// Decodes a pasted or scanned payload and validates it against the outstanding
    /// request.
    pub fn decode_response(&mut self, bytes: &[u8]) -> Result<QrResponse, Error> {
        match bwk_qr_protocol::decode(bytes)? {
            Message::Request(_) => Err(Error::PayloadIsRequest),
            Message::Response(response) => self.validate_response(response),
        }
    }

    fn validate_response(&mut self, response: Response) -> Result<QrResponse, Error> {
        let Some(outstanding) = self.outstanding.as_ref() else {
            return Err(Error::NoOutstandingRequest);
        };
        if response.id != outstanding.id {
            return Err(Error::RequestIdMismatch {
                expected: hex_id(outstanding.id),
                got: hex_id(response.id),
            });
        }
        let expected_type = outstanding.message_type;
        let expected_address = outstanding.expected_address.clone();
        let original_psbt_v2 = outstanding.original_psbt_v2.clone();

        let body = response.body;
        if let response::Body::Error(e) = body {
            return Err(Error::SignerRefused {
                message_type: e.message_type,
                code: e.error,
                message: e.message,
            });
        }
        let got_type = body.message_type();
        if got_type != expected_type {
            return Err(Error::ResponseKindMismatch {
                expected: expected_type,
                got: got_type,
            });
        }

        let parsed = match body {
            response::Body::Keys(keys) => {
                let requested = outstanding
                    .keys
                    .as_ref()
                    .expect("a GetKeys request always records its keys");
                validate_keys(keys, requested)?
            }
            response::Body::Signed(signed) => validate_signed(signed, original_psbt_v2.as_deref())?,
            response::Body::Registration(registration) => validate_registration(registration)?,
            response::Body::AddressUri(address_uri) => {
                validate_address_uri(address_uri, expected_address)?
            }
            response::Body::Error(_) => unreachable!("error responses are handled above"),
        };

        self.outstanding = None;
        Ok(parsed)
    }
}

/// A decoded and validated signing-flow QR response.
pub enum QrResponse {
    Xpubs {
        fingerprint: Fingerprint,
        xpubs: Vec<(DerivationPath, Xpub)>,
        vendor: String,
        model: String,
        version: String,
        capabilities: u32,
    },
    Registration {
        alias: String,
        registered: bool,
    },
    /// Whether the device's independently derived address matched the one the
    /// caller asked it to confirm.
    AddressVerified(bool),
    /// The original v2 PSBT the sign request carried, with the device's signatures
    /// merged in. Always v2, even for a plain (non-SP) sign request that was sent to
    /// the device as v0.
    SignedPsbt(Vec<u8>),
    /// A silent payment account: its watch-only sp() descriptor, built from the keys
    /// the signer gave, and its taproot and segwit sub-accounts' xpubs.
    SpAccount {
        descriptor: String,
        fingerprint: Fingerprint,
        xpubs: Vec<(DerivationPath, Xpub)>,
    },
}

/// A `Keys` response must carry one key per requested key, each of the requested kind.
fn validate_keys(keys: response::Keys, requested: &KeysRequest) -> Result<QrResponse, Error> {
    let requested_keys = requested.keys();
    if keys.keys.len() != requested_keys.len() {
        return Err(Error::XpubCountMismatch {
            requested: requested_keys.len(),
            returned: keys.keys.len(),
        });
    }
    let fingerprint = Fingerprint::from(&keys.fingerprint);
    match requested {
        KeysRequest::Xpubs(paths) => {
            let mut xpubs = Vec::with_capacity(paths.len());
            for (path, key) in paths.iter().zip(keys.keys.iter()) {
                xpubs.push((path.clone(), public_key(key, path)?));
            }
            Ok(QrResponse::Xpubs {
                fingerprint,
                xpubs,
                vendor: keys.vendor,
                model: keys.model,
                version: format!(
                    "{}.{}.{}",
                    keys.version.major, keys.version.minor, keys.version.patch
                ),
                capabilities: keys.capabilities.0,
            })
        }
        KeysRequest::SpAccount(paths) => {
            let keys = keys
                .keys
                .try_into()
                .expect("the key count is checked above");
            sp_account_response(fingerprint, keys, paths)
        }
    }
}

/// `keys` are in `SpAccountPaths` request order.
fn sp_account_response(
    fingerprint: Fingerprint,
    [spend, scan, taproot, segwit]: [types::ExtendedKey; 4],
    paths: &SpAccountPaths,
) -> Result<QrResponse, Error> {
    let spend = public_key(&spend, &paths.spend)?;
    let scan = private_key(&scan, &paths.scan)?;
    let taproot = public_key(&taproot, &paths.taproot)?;
    let segwit = public_key(&segwit, &paths.segwit)?;
    let descriptor = watch_only_sp_descriptor(
        fingerprint,
        paths.network,
        paths.account,
        scan.private_key,
        spend.public_key,
    );
    Ok(QrResponse::SpAccount {
        descriptor: descriptor.to_string(),
        fingerprint,
        xpubs: vec![
            (paths.taproot.clone(), taproot),
            (paths.segwit.clone(), segwit),
        ],
    })
}

/// The xpub the signer returned for a public key request at `path`.
fn public_key(key: &types::ExtendedKey, path: &DerivationPath) -> Result<Xpub, Error> {
    Xpub::try_from(key).map_err(|e| match Xpriv::try_from(key) {
        Ok(_) => Error::KeyKindMismatch {
            path: path.clone(),
            requested: KeyKind::Public,
        },
        Err(_) => Error::from(e),
    })
}

/// The xpriv the signer returned for a private key request at `path`.
fn private_key(key: &types::ExtendedKey, path: &DerivationPath) -> Result<Xpriv, Error> {
    Xpriv::try_from(key).map_err(|e| match Xpub::try_from(key) {
        Ok(_) => Error::KeyKindMismatch {
            path: path.clone(),
            requested: KeyKind::Private,
        },
        Err(_) => Error::from(e),
    })
}

/// Merges the device's answer into `original_psbt_v2` with the same helper USB
/// signing uses, so both paths always hand back v2 bytes with the original's silent
/// payment fields intact. `original_psbt_v2` is `None` for a signed response to a
/// request that carried no PSBT, which is refused rather than trusted as-is.
fn validate_signed(
    signed: response::Signed,
    original_psbt_v2: Option<&[u8]>,
) -> Result<QrResponse, Error> {
    match signed {
        response::Signed::Psbt(bytes) => {
            let original = original_psbt_v2.ok_or(Error::InvalidSignedPsbt)?;
            let merged = psbt::merge_signatures_bytes(original, &bytes)?;
            Ok(QrResponse::SignedPsbt(merged))
        }
        response::Signed::Signatures(_) => Err(Error::SignaturesUnsupported),
    }
}

fn validate_registration(registration: response::Registration) -> Result<QrResponse, Error> {
    let registered = registration
        .registered
        .ok_or(Error::IncompleteRegistration)?;
    Ok(QrResponse::Registration {
        alias: registration.descriptor_alias,
        registered,
    })
}

/// The device echoes back the address it independently derived as `bitcoin:<address>`,
/// never the bare address; comparing it against `expected_address` is the whole
/// point of the request.
/// The keys a signer rebuilds an sp() descriptor's address from, with no secret: its
/// fingerprint and the BIP-352 spend and scan paths of its account.
fn sp_keys(descriptor: &SpDescriptor) -> Result<SpKeys, Error> {
    let (fingerprint, origin) = descriptor.origin().ok_or(Error::SpDescriptorNoOrigin)?;
    // The origin is the account, `352'/coin'/account'`, or a key under it.
    let steps: Vec<ChildNumber> = origin.clone().into();
    let purpose = ChildNumber::from_hardened_idx(352).expect("352 is a valid index");
    let (Some(&first), Some(&coin), Some(&account)) = (steps.first(), steps.get(1), steps.get(2))
    else {
        return Err(Error::SpAccountPath(origin));
    };
    if first != purpose || !coin.is_hardened() || !account.is_hardened() {
        return Err(Error::SpAccountPath(origin));
    }
    // The paths only tell mainnet (coin 0') from the test networks (coin 1').
    let network = match coin {
        ChildNumber::Hardened { index: 0 } => Network::Bitcoin,
        _ => Network::Testnet,
    };
    Ok(SpKeys {
        fingerprint,
        spend_path: spend_path(network, account),
        scan_path: scan_path(network, account),
    })
}

fn validate_address_uri(
    address_uri: response::AddressUri,
    expected_address: Option<String>,
) -> Result<QrResponse, Error> {
    let uri = address_uri.uri.ok_or(Error::NoAddressUri)?;
    let expected = expected_address.ok_or(Error::MissingExpectedAddress)?;
    let address = uri.strip_prefix("bitcoin:").unwrap_or(&uri);
    Ok(QrResponse::AddressVerified(address == expected))
}

/// Hex-encodes a request id for error messages.
fn hex_id(id: RequestId) -> String {
    id.0.iter().map(|byte| format!("{byte:02x}")).collect()
}

/// One request as issued: its id and the protocol message carrying it.
pub struct QrRequest {
    id: RequestId,
    message: Vec<u8>,
}

impl QrRequest {
    pub fn id(&self) -> RequestId {
        self.id
    }

    pub fn message(&self) -> &[u8] {
        &self.message
    }

    pub fn into_message(self) -> Vec<u8> {
        self.message
    }
}
