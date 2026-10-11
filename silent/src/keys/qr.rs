//! The bwk-qr protocol codec: builds signing-flow requests as protocol messages, and
//! decodes and validates the matching responses. Framing the bytes is the transport's
//! business.
//!
//! `QrSession` owns the request id counter and remembers the one outstanding request,
//! so a decoded response is checked against what was actually asked before it is
//! accepted.

use std::str::FromStr;

use bitcoin::bip32::{DerivationPath, Fingerprint, Xpub};
use bwk_qr_protocol::{
    request, response, Message, MessageType, Request, RequestId, Response, SignResponseKind,
};
use bwk_sp::bwk_sign::bwk_descriptor::sp_descriptor::SpDescriptor;

use crate::hwi;
use crate::keys::error::Error;
use crate::keys::psbt;

/// What the next response has to match. `requested_paths` is empty for every
/// request but `GetXpubs`, whose response carries xpubs in request order and
/// nothing else to pair them with. Read back by `validate_response` to check a
/// decoded response against what was actually asked.
struct Outstanding {
    id: RequestId,
    message_type: MessageType,
    requested_paths: Vec<DerivationPath>,
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
        let derivation_paths = paths
            .iter()
            .map(bwk_qr_protocol::types::DerivationPath::from)
            .collect();
        let body = request::Body::GetXpubs(request::GetXpubs { derivation_paths });
        self.issue(body, paths, None, None)
    }

    pub fn register_descriptor(&mut self, descriptor: &str) -> Result<QrRequest, Error> {
        let body = request::Body::RegisterDescriptor(request::RegisterDescriptor {
            descriptor_alias: hwi::wallet_name(descriptor),
            descriptor: Some(request::DescriptorBody::Bip380(descriptor.to_string())),
        });
        self.issue(body, Vec::new(), None, None)
    }

    pub fn verify_address(
        &mut self,
        descriptor: &str,
        path: DerivationPath,
        address: String,
    ) -> Result<QrRequest, Error> {
        let body = request::Body::VerifyAddress(request::VerifyAddress {
            descriptor_alias: hwi::wallet_name(descriptor),
            derivation_path: bwk_qr_protocol::types::DerivationPath::from(&path),
            address: Some(address.clone()),
            descriptor: Some(request::DescriptorBody::Bip380(descriptor.to_string())),
            proof: None,
        });
        self.issue(body, Vec::new(), Some(address), None)
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
        self.issue(body, Vec::new(), None, Some(psbt_v2_bytes.to_vec()))
    }

    pub fn get_sp_descriptor(&mut self) -> Result<QrRequest, Error> {
        let body = request::Body::GetSpDescriptor(request::GetSpDescriptor {});
        self.issue(body, Vec::new(), None, None)
    }

    /// Encodes the request and makes it the outstanding one.
    fn issue(
        &mut self,
        body: request::Body,
        requested_paths: Vec<DerivationPath>,
        expected_address: Option<String>,
        original_psbt_v2: Option<Vec<u8>>,
    ) -> Result<QrRequest, Error> {
        let id = self.next_id();
        let message_type = body.message_type();
        let message = bwk_qr_protocol::encode_request(&Request { id, body })?;
        self.outstanding = Some(Outstanding {
            id,
            message_type,
            requested_paths,
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
        let requested_paths = outstanding.requested_paths.clone();
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
            response::Body::Xpubs(xpubs) => validate_xpubs(xpubs, &requested_paths)?,
            response::Body::Signed(signed) => validate_signed(signed, original_psbt_v2.as_deref())?,
            response::Body::SpDescriptor(descriptor) => {
                validate_sp_descriptor_response(descriptor)?
            }
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
        model: String,
        version: String,
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
    SpDescriptor {
        descriptor: String,
        fingerprint: Fingerprint,
    },
}

fn validate_xpubs(
    xpubs: response::Xpubs,
    requested_paths: &[DerivationPath],
) -> Result<QrResponse, Error> {
    if xpubs.xpubs.len() != requested_paths.len() {
        return Err(Error::XpubCountMismatch {
            requested: requested_paths.len(),
            returned: xpubs.xpubs.len(),
        });
    }
    let mut converted = Vec::with_capacity(xpubs.xpubs.len());
    for (path, xpub) in requested_paths.iter().zip(xpubs.xpubs.iter()) {
        converted.push((path.clone(), Xpub::try_from(xpub)?));
    }
    Ok(QrResponse::Xpubs {
        fingerprint: Fingerprint::from(&xpubs.fingerprint),
        xpubs: converted,
        model: xpubs.model,
        version: format!(
            "{}.{}.{}",
            xpubs.version.major, xpubs.version.minor, xpubs.version.patch
        ),
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

fn validate_sp_descriptor_response(
    descriptor: response::SpDescriptor,
) -> Result<QrResponse, Error> {
    let parsed = SpDescriptor::from_str(&descriptor.descriptor)
        .map_err(|e| Error::DescriptorParse(e.to_string()))?;
    let fingerprint = parsed.fingerprint().ok_or(Error::SpDescriptorNoOrigin)?;
    Ok(QrResponse::SpDescriptor {
        descriptor: descriptor.descriptor,
        fingerprint,
    })
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
