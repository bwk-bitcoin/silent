//! Builds signing-flow QR requests and encodes them into animated frame sets.
//!
//! `QrSession` owns the request id counter and remembers the one outstanding request,
//! so the response side (a later task) can check a decoded response against what was
//! actually asked. Response decoding is not implemented here.

use bitcoin::bip32::DerivationPath;
use bwk_qr_protocol::{request, MessageType, Request, RequestId, SignResponseKind};

use crate::hwi;
use crate::keys::error::Error;
use crate::keys::psbt;

/// What the next response has to match. `requested_paths` is empty for every
/// request but `GetXpubs`, whose response carries xpubs in request order and
/// nothing else to pair them with. Read back by the response side in a later task.
struct Outstanding {
    id: RequestId,
    #[allow(dead_code)]
    message_type: MessageType,
    #[allow(dead_code)]
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
