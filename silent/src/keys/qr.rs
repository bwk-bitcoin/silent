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
use bwk_sp::{bwk_sign::bwk_descriptor::sp_descriptor::SpDescriptor, bwk_tx::bwk_psbt::PsbtV2};

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
        self.issue(body, paths, part_bytes)
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
        self.issue(body, Vec::new(), part_bytes)
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
            address: Some(address),
            descriptor: Some(request::DescriptorBody::Bip380(descriptor.to_string())),
            proof: None,
        });
        self.issue(body, Vec::new(), part_bytes)
    }

    pub fn sign(
        &mut self,
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
        // conversion drops, so it goes over the wire unchanged. A plain hardware
        // signer over QR still expects v0.
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
        self.issue(body, Vec::new(), part_bytes)
    }

    pub fn get_sp_descriptor(&mut self, part_bytes: usize) -> Result<QrRequest, Error> {
        let body = request::Body::GetSpDescriptor(request::GetSpDescriptor {});
        self.issue(body, Vec::new(), part_bytes)
    }

    fn issue(
        &mut self,
        body: request::Body,
        requested_paths: Vec<DerivationPath>,
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
            let config = bwk_qr::config::Config {
                bbqr_part_bytes: target_part_bytes,
                ..bwk_qr::config::Config::default()
            };
            self.encoder = bwk_qr::encoder::Encoder::new(config)?;
            self.encoder_part_bytes = target_part_bytes;
        }
        let frames = self.encoder.encode_request(&request)?;
        if frames.is_empty() {
            return Err(Error::NoFrames);
        }
        self.outstanding = Some(Outstanding {
            id,
            message_type,
            requested_paths,
        });
        Ok(QrRequest {
            id,
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
            response::Body::Signed(signed) => validate_signed(signed)?,
            response::Body::SpDescriptor(descriptor) => {
                validate_sp_descriptor_response(descriptor)?
            }
            response::Body::Registration(registration) => validate_registration(registration)?,
            response::Body::AddressUri(address_uri) => validate_address_uri(address_uri)?,
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
    AddressUri(String),
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

fn validate_signed(signed: response::Signed) -> Result<QrResponse, Error> {
    match signed {
        response::Signed::Psbt(bytes) => {
            if PsbtV2::deserialize(&bytes).is_err() && bitcoin::Psbt::deserialize(&bytes).is_err() {
                return Err(Error::InvalidSignedPsbt);
            }
            Ok(QrResponse::SignedPsbt(bytes))
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

fn validate_address_uri(address_uri: response::AddressUri) -> Result<QrResponse, Error> {
    let uri = address_uri.uri.ok_or(Error::NoAddressUri)?;
    Ok(QrResponse::AddressUri(uri))
}

/// Hex-encodes a request id for error messages.
fn hex_id(id: RequestId) -> String {
    id.0.iter().map(|byte| format!("{byte:02x}")).collect()
}

/// One encoded QR request: the protocol message and the full set of animation frames
/// a caller pulls from on its own timer.
pub struct QrRequest {
    id: RequestId,
    message: Vec<u8>,
    frames: Vec<bwk_qr::image::Image>,
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
