//! Builds signing-flow QR requests, encodes them into animated frame sets, and
//! decodes and validates the matching responses.
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

/// Allocates request ids, builds the five signing-flow requests, and encodes each
/// into its animated frame set. One encoder per session, built once from the default
/// config and reused unless a request asks for a different BBQR part size.
pub struct QrSession {
    counter: u16,
    outstanding: Option<Outstanding>,
    encoder: bwk_qr::encoder::Encoder,
    encoder_part_bytes: usize,
}

impl QrSession {
    pub fn new() -> Result<Self, Error> {
        let config = bwk_qr::config::Config::default();
        let encoder_part_bytes = config.bbqr_part_bytes;
        let encoder = bwk_qr::encoder::Encoder::new(config)?;
        Ok(Self {
            counter: 0,
            outstanding: None,
            encoder,
            encoder_part_bytes,
        })
    }

    /// Reinitializes the session in place for a network switch: clears any
    /// outstanding request and rebuilds the id counter and encoder. Keeps the
    /// session's identity intact, so a `KeysReceiver` taken before the switch
    /// (holding its own `Arc` clone) keeps observing this same session instead
    /// of a stale, disconnected one.
    pub fn reset(&mut self) -> Result<(), Error> {
        *self = Self::new()?;
        Ok(())
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

    /// Reserves the id a request will be built under, without building it. Lets a
    /// caller hand the id to a builder running off the session (`build_sign_request`)
    /// and commit the result back later with `commit`.
    pub fn reserve_id(&mut self) -> RequestId {
        self.next_id()
    }

    /// Commits a request built out-of-band (by `build_sign_request`, on any thread)
    /// as the session's outstanding request. Refused if a newer id has been reserved
    /// since, so a superseded density rebuild never clobbers the request the caller
    /// actually still wants answered.
    pub fn commit(&mut self, request: &QrRequest) -> Result<(), Error> {
        let latest = RequestId(self.counter.to_be_bytes());
        if request.id != latest {
            return Err(Error::StaleRequest {
                expected: latest,
                got: request.id,
            });
        }
        self.outstanding = Some(Outstanding {
            id: request.id,
            message_type: request.message_type,
            requested_paths: request.requested_paths.clone(),
            expected_address: request.expected_address.clone(),
            original_psbt_v2: request.original_psbt_v2.clone(),
        });
        Ok(())
    }

    pub fn get_xpubs(
        &mut self,
        paths: Vec<DerivationPath>,
        part_bytes: usize,
    ) -> Result<QrRequest, Error> {
        if paths.is_empty() {
            return Err(Error::EmptyPathList);
        }
        let derivation_paths = paths
            .iter()
            .map(bwk_qr_protocol::types::DerivationPath::from)
            .collect();
        let body = request::Body::GetXpubs(request::GetXpubs { derivation_paths });
        self.issue(body, paths, None, part_bytes)
    }

    pub fn register_descriptor(
        &mut self,
        descriptor: &str,
        part_bytes: usize,
    ) -> Result<QrRequest, Error> {
        let body = request::Body::RegisterDescriptor(request::RegisterDescriptor {
            descriptor_alias: hwi::wallet_name(descriptor),
            descriptor: Some(request::DescriptorBody::Bip380(descriptor.to_string())),
        });
        self.issue(body, Vec::new(), None, part_bytes)
    }

    pub fn verify_address(
        &mut self,
        descriptor: &str,
        path: DerivationPath,
        address: String,
        part_bytes: usize,
    ) -> Result<QrRequest, Error> {
        let body = request::Body::VerifyAddress(request::VerifyAddress {
            descriptor_alias: hwi::wallet_name(descriptor),
            derivation_path: bwk_qr_protocol::types::DerivationPath::from(&path),
            address: Some(address.clone()),
            descriptor: Some(request::DescriptorBody::Bip380(descriptor.to_string())),
            proof: None,
        });
        self.issue(body, Vec::new(), Some(address), part_bytes)
    }

    /// Reserves an id, builds the request off the session, and commits it back in one
    /// call: the synchronous convenience path for a caller that does not need the
    /// build to run off the GUI thread (tests, and the other four QR builders' style).
    pub fn sign(
        &mut self,
        descriptors: &[String],
        psbt_v2_bytes: &[u8],
        part_bytes: usize,
    ) -> Result<QrRequest, Error> {
        let id = self.reserve_id();
        let request = build_sign_request(id, descriptors, psbt_v2_bytes, part_bytes)?;
        self.commit(&request)?;
        Ok(request)
    }

    pub fn get_sp_descriptor(&mut self, part_bytes: usize) -> Result<QrRequest, Error> {
        let body = request::Body::GetSpDescriptor(request::GetSpDescriptor {});
        self.issue(body, Vec::new(), None, part_bytes)
    }

    fn issue(
        &mut self,
        body: request::Body,
        requested_paths: Vec<DerivationPath>,
        expected_address: Option<String>,
        part_bytes: usize,
    ) -> Result<QrRequest, Error> {
        let id = self.next_id();
        let message_type = body.message_type();
        let request = Request { id, body };
        let message = bwk_qr_protocol::encode_request(&request)?;
        let target_part_bytes = if part_bytes == 0 {
            bwk_qr::config::Config::default().bbqr_part_bytes
        } else {
            part_bytes
        };
        if target_part_bytes != self.encoder_part_bytes {
            self.encoder = encoder_for(target_part_bytes)?;
            self.encoder_part_bytes = target_part_bytes;
        }
        let frames = self.encoder.encode_request(&request)?;
        if frames.is_empty() {
            return Err(Error::NoFrames);
        }
        self.outstanding = Some(Outstanding {
            id,
            message_type,
            requested_paths: requested_paths.clone(),
            expected_address: expected_address.clone(),
            original_psbt_v2: None,
        });
        Ok(QrRequest {
            id,
            message_type,
            requested_paths,
            expected_address,
            original_psbt_v2: None,
            message,
            frames,
        })
    }

    /// Decodes a pasted or scanned payload and validates it against the outstanding
    /// request.
    pub fn decode_response(&mut self, bytes: &[u8]) -> Result<QrResponse, Error> {
        match bwk_qr_protocol::decode(bytes)? {
            Message::Request(_) => Err(Error::PayloadIsRequest),
            Message::Response(response) => self.validate_response(response),
        }
    }

    /// Validates a response the camera already protocol-decoded. `decode_response` is
    /// this function plus `bwk_qr_protocol::decode`, so a scanned payload and a pasted
    /// one go through exactly the same checks.
    pub fn validate_response(&mut self, response: Response) -> Result<QrResponse, Error> {
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

/// QR versions a frame can be pinned at.
const QR_VERSIONS: std::ops::RangeInclusive<u8> = 1..=40;

/// An encoder pinned at the smallest QR version a full part of `part_bytes` fits, so
/// a smaller part draws a coarser code: bwk-qr renders every part at the pinned
/// version, and its default (27) would draw a short request as a dense 125-module code.
fn encoder_for(part_bytes: usize) -> Result<bwk_qr::encoder::Encoder, Error> {
    let config = |max_qr_version| bwk_qr::config::Config {
        max_qr_version,
        bbqr_part_bytes: part_bytes,
        ..bwk_qr::config::Config::default()
    };
    if let Some(encoder) = QR_VERSIONS
        .clone()
        .find_map(|version| bwk_qr::encoder::Encoder::new(config(version)).ok())
    {
        return Ok(encoder);
    }
    Ok(bwk_qr::encoder::Encoder::new(config(*QR_VERSIONS.end()))?)
}

/// One encoded QR request: the protocol message and the full set of animation frames
/// a caller pulls from on its own timer. Carries what `commit` needs to turn it into
/// the session's `Outstanding` request, so a request built off the session (by
/// `build_sign_request`) is self-sufficient once handed back to `QrSession::commit`.
pub struct QrRequest {
    id: RequestId,
    message_type: MessageType,
    requested_paths: Vec<DerivationPath>,
    expected_address: Option<String>,
    /// The original v2 PSBT bytes, for a `Sign` request. `None` for every other
    /// builder. Carried into `Outstanding` by `commit`, so `validate_signed` can
    /// merge the device's answer into it.
    original_psbt_v2: Option<Vec<u8>>,
    message: Vec<u8>,
    frames: Vec<bwk_qr::image::Image>,
}

/// Builds a signing-flow QR request without touching a `QrSession`: no encoder reuse,
/// no outstanding-request tracking, safe to call from any thread. `id` comes from
/// `QrSession::reserve_id` beforehand; the result is handed to `QrSession::commit`
/// afterwards, on whichever thread owns the session. This is the one QR builder meant
/// to run off the GUI thread, since a large PSBT makes encoding it non-trivial work.
pub fn build_sign_request(
    id: RequestId,
    descriptors: &[String],
    psbt_v2_bytes: &[u8],
    part_bytes: usize,
) -> Result<QrRequest, Error> {
    let request_descriptors = descriptors
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
        descriptors: request_descriptors,
        psbt: payload,
        want_kind: Some(SignResponseKind::Psbt),
    });
    let message_type = body.message_type();
    let request = Request { id, body };
    let message = bwk_qr_protocol::encode_request(&request)?;

    let target_part_bytes = if part_bytes == 0 {
        bwk_qr::config::Config::default().bbqr_part_bytes
    } else {
        part_bytes
    };
    let encoder = encoder_for(target_part_bytes)?;
    let frames = encoder.encode_request(&request)?;
    if frames.is_empty() {
        return Err(Error::NoFrames);
    }

    Ok(QrRequest {
        id,
        message_type,
        requested_paths: Vec::new(),
        expected_address: None,
        original_psbt_v2: Some(psbt_v2_bytes.to_vec()),
        message,
        frames,
    })
}

impl QrRequest {
    pub fn id(&self) -> RequestId {
        self.id
    }

    /// The encoded protocol message, for the paste box and for tests.
    pub fn message(&self) -> &[u8] {
        &self.message
    }

    pub fn frame_count(&self) -> usize {
        self.frames.len()
    }

    pub fn frame(&self, index: usize) -> Result<&bwk_qr::image::Image, Error> {
        self.frames.get(index).ok_or(Error::FrameOutOfRange {
            index,
            count: self.frames.len(),
        })
    }
}

#[cfg(test)]
mod tests {
    use bitcoin::{absolute::LockTime, hashes::Hash, transaction};
    use bwk_sp::bwk_tx::bwk_psbt::{Input as PsbtInput, Output as PsbtOutput, PsbtV2};

    use crate::keys::error::Error;
    use crate::keys::qr::{build_sign_request, QrSession};

    fn sign_psbt_bytes() -> Vec<u8> {
        let psbt = PsbtV2 {
            tx_version: transaction::Version::TWO,
            fallback_lock_time: Some(LockTime::ZERO),
            tx_modifiable: None,
            xpub: Default::default(),
            proprietary: Default::default(),
            unknown: Default::default(),
            inputs: vec![PsbtInput {
                previous_output: bitcoin::OutPoint::new(Hash::all_zeros(), 0),
                sequence: bitcoin::Sequence::MAX,
                required_time_lock_time: None,
                required_height_lock_time: None,
                psbt: bitcoin::psbt::Input::default(),
            }],
            outputs: vec![PsbtOutput {
                amount: bitcoin::Amount::from_sat(1_000),
                script_pubkey: Some(bitcoin::ScriptBuf::new()),
                psbt: bitcoin::psbt::Output::default(),
            }],
        };
        psbt.serialize().expect("serialize psbt")
    }

    #[test]
    fn commit_rejects_a_stale_request() {
        let mut session = QrSession::new().expect("create session");
        let stale_id = session.reserve_id();
        let request =
            build_sign_request(stale_id, &["wpkh(desc)".to_string()], &sign_psbt_bytes(), 0)
                .expect("build sign request");
        session.reserve_id();

        let error = session
            .commit(&request)
            .expect_err("a stale commit must be rejected");
        assert!(
            matches!(error, Error::StaleRequest { .. }),
            "expected StaleRequest, got {error:?}"
        );
    }

    #[test]
    fn commit_sets_the_latest_outstanding_id() {
        let mut session = QrSession::new().expect("create session");
        let id = session.reserve_id();
        let request = build_sign_request(id, &["wpkh(desc)".to_string()], &sign_psbt_bytes(), 0)
            .expect("build sign request");

        session.commit(&request).expect("commit succeeds");
        assert_eq!(session.outstanding_id(), Some(id));
    }
}
