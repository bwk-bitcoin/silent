//! BBQR transport for the core QR pipeline, protocol agnostic: a payload is split into
//! animated QR frames, and camera frames are scanned and joined back into payloads.
//! What the payload means is the codec's business.

use bwk_qr::{
    config::Config,
    decoder::{Decoded, Decoder},
    encoder::Encoder,
    error::Error,
    image::Image,
};

use crate::{
    ffi,
    image::{box_downscale, downscale_factor, rgb_to_luma, RGB_CHANNELS},
};

/// QR versions a frame can be pinned at.
const QR_VERSIONS: std::ops::RangeInclusive<u8> = 1..=40;

/// An encoder pinned at the smallest QR version a full part of `part_bytes` fits, so
/// a smaller part draws a coarser code: every part renders at the pinned version, and
/// the default (27) would draw a short payload as a dense 125-module code.
fn encoder_for(part_bytes: usize) -> Result<Encoder, Error> {
    let config = |max_qr_version| Config {
        max_qr_version,
        bbqr_part_bytes: part_bytes,
        ..Config::default()
    };
    if let Some(encoder) = QR_VERSIONS
        .clone()
        .find_map(|version| Encoder::new(config(version)).ok())
    {
        return Ok(encoder);
    }
    Encoder::new(config(*QR_VERSIONS.end()))
}

/// Splits `payload` into BBQR parts of `part_bytes`, one QR image each.
pub fn frames(payload: &[u8], part_bytes: usize) -> Result<Vec<Image>, Error> {
    encoder_for(part_bytes)?.encode_bytes(payload)
}

/// What one scanned frame gave: the payloads it completed and the join progress.
pub struct ScanOutcome {
    pub payloads: Vec<Vec<u8>>,
    pub seen: usize,
    pub total: usize,
}

/// Joins BBQR parts read off successive RGB888 frames. Stateful: the parts seen so far
/// stay until a payload completes or `reset` drops them.
pub struct Scanner {
    decoder: Decoder,
    /// A plain code's text is a payload of its own, as an address code is.
    text: bool,
}

impl Scanner {
    pub fn new() -> Result<Self, Error> {
        Ok(Self {
            decoder: Decoder::new(Config::default())?,
            text: false,
        })
    }

    /// A scanner that also hands back the text of plain codes.
    pub fn with_text() -> Result<Self, Error> {
        Ok(Self {
            text: true,
            ..Self::new()?
        })
    }

    /// A frame that holds no readable part, or an unreadable one, completes nothing:
    /// a camera frame routinely does.
    pub fn scan(&mut self, rgb: &[u8], width: u32, height: u32) -> ScanOutcome {
        // The scanner refuses a frame above its pixel limit as a bad frame.
        let factor = downscale_factor(width, height, Config::default().max_image_pixels);
        let luma = box_downscale(&rgb_to_luma(rgb), width, height, 1, factor);
        let decoded = self
            .decoder
            .process_raw(&Image {
                data: luma.data,
                width: luma.width,
                height: luma.height,
            })
            .unwrap_or_else(|e| {
                log::debug!("bbqr: unreadable frame: {e}");
                Vec::new()
            });
        let text = self.text;
        let payloads = decoded
            .into_iter()
            .filter_map(|decoded| match decoded {
                Decoded::Bytes(payload) => Some(payload),
                Decoded::Text(content) if text => Some(content.into_bytes()),
                // A plain code is not a BBQR part: nothing to join.
                _ => None,
            })
            .collect();
        let (seen, total) = self
            .decoder
            .progress()
            .map_or((0, 0), |progress| (progress.seen, progress.total));
        ScanOutcome {
            payloads,
            seen,
            total,
        }
    }

    pub fn reset(&mut self) {
        self.decoder.reset();
    }
}

// ===== Bridge =====

/// The frames of one payload, or why it could not be framed.
pub struct BbqrFrames {
    inner: Result<Vec<Image>, String>,
}

impl BbqrFrames {
    pub fn is_ok(&self) -> bool {
        self.inner.is_ok()
    }

    pub fn get_error(&self) -> String {
        self.inner.as_ref().err().cloned().unwrap_or_default()
    }

    pub fn count(&self) -> u32 {
        self.inner.as_ref().map_or(0, |frames| frames.len() as u32)
    }

    /// An index past the end answers with is_ok false.
    pub fn frame(&self, index: u32) -> ffi::QrImageResult {
        let image = match self.inner.as_ref() {
            Ok(frames) => frames.get(index as usize),
            Err(e) => return failed_image(e),
        };
        match image {
            Some(image) => ffi::QrImageResult {
                is_ok: true,
                error: String::new(),
                data: image.data.clone(),
                width: image.width,
                height: image.height,
            },
            None => failed_image(&format!("frame {index} is out of range")),
        }
    }
}

/// Stateless and touching nothing shared: safe on any thread.
pub fn bbqr_frames(payload: &[u8], part_bytes: u32) -> Box<BbqrFrames> {
    Box::new(BbqrFrames {
        inner: frames(payload, part_bytes as usize).map_err(|e| e.to_string()),
    })
}

/// A `Scanner`, or why it could not be built.
pub struct BbqrScanner {
    inner: Result<Scanner, String>,
}

impl BbqrScanner {
    pub fn is_ok(&self) -> bool {
        self.inner.is_ok()
    }

    pub fn get_error(&self) -> String {
        self.inner.as_ref().err().cloned().unwrap_or_default()
    }

    /// `rgb` is RGB888, `width * height * 3` bytes.
    pub fn scan(&mut self, rgb: &[u8], width: u32, height: u32) -> ffi::BbqrScanResult {
        let scanner = match self.inner.as_mut() {
            Ok(scanner) => scanner,
            Err(e) => return failed_scan(e),
        };
        if rgb.len() != (width * height * RGB_CHANNELS) as usize {
            return failed_scan(&format!(
                "frame of {} bytes is not {width}x{height} RGB888",
                rgb.len()
            ));
        }
        let outcome = scanner.scan(rgb, width, height);
        ffi::BbqrScanResult {
            is_ok: true,
            error: String::new(),
            payloads: outcome
                .payloads
                .into_iter()
                .map(|data| ffi::QrPayload { data })
                .collect(),
            seen: outcome.seen as u32,
            total: outcome.total as u32,
        }
    }

    pub fn reset(&mut self) {
        if let Ok(scanner) = self.inner.as_mut() {
            scanner.reset();
        }
    }
}

pub fn bbqr_scanner_new() -> Box<BbqrScanner> {
    Box::new(BbqrScanner {
        inner: Scanner::new().map_err(|e| e.to_string()),
    })
}

pub fn bbqr_text_scanner_new() -> Box<BbqrScanner> {
    Box::new(BbqrScanner {
        inner: Scanner::with_text().map_err(|e| e.to_string()),
    })
}

fn failed_image(error: &str) -> ffi::QrImageResult {
    ffi::QrImageResult {
        is_ok: false,
        error: error.to_string(),
        data: Vec::new(),
        width: 0,
        height: 0,
    }
}

fn failed_scan(error: &str) -> ffi::BbqrScanResult {
    ffi::BbqrScanResult {
        is_ok: false,
        error: error.to_string(),
        payloads: Vec::new(),
        seen: 0,
        total: 0,
    }
}

#[cfg(test)]
mod tests {
    use crate::bbqr::{frames, Scanner};
    use bwk_qr::{config::Config, encoder::Encoder, image::Image};

    fn payload() -> Vec<u8> {
        (0..=255u8).cycle().take(300).collect()
    }

    /// A grayscale QR image as the RGB888 frame a camera would hand over.
    fn to_rgb(image: &Image) -> Vec<u8> {
        image.data.iter().flat_map(|&luma| [luma; 3]).collect()
    }

    #[test]
    fn payload_round_trips_through_frames_and_the_scanner() {
        let frames = frames(&payload(), 50).expect("payload frames");
        assert!(
            frames.len() > 1,
            "300 bytes in 50 byte parts should take several frames, got {}",
            frames.len()
        );

        let mut scanner = Scanner::new().expect("scanner");
        let mut payloads = Vec::new();
        for (index, frame) in frames.iter().enumerate() {
            let outcome = scanner.scan(&to_rgb(frame), frame.width, frame.height);
            if outcome.payloads.is_empty() {
                assert_eq!(
                    (outcome.seen, outcome.total),
                    (index + 1, frames.len()),
                    "frame {index}: parts seen of the total"
                );
            }
            payloads.extend(outcome.payloads);
        }
        assert_eq!(
            payloads,
            vec![payload()],
            "the joined payload is the original"
        );
    }

    fn address_code() -> Image {
        Encoder::new(Config::default())
            .expect("encoder")
            .encode_text(ADDRESS)
            .expect("address code")
    }

    const ADDRESS: &str = "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080";

    #[test]
    fn a_text_scanner_reads_a_plain_code() {
        let code = address_code();
        let mut scanner = Scanner::with_text().expect("scanner");
        let outcome = scanner.scan(&to_rgb(&code), code.width, code.height);
        assert_eq!(
            outcome.payloads,
            vec![ADDRESS.as_bytes().to_vec()],
            "the plain code's text is the payload"
        );
    }

    #[test]
    fn a_bbqr_scanner_ignores_a_plain_code() {
        let code = address_code();
        let mut scanner = Scanner::new().expect("scanner");
        let outcome = scanner.scan(&to_rgb(&code), code.width, code.height);
        assert!(
            outcome.payloads.is_empty(),
            "a plain code is no BBQR part, got {:?}",
            outcome.payloads
        );
    }

    #[test]
    fn reset_drops_the_parts_seen() {
        let frames = frames(&payload(), 50).expect("payload frames");
        let mut scanner = Scanner::new().expect("scanner");
        let first = &frames[0];
        let outcome = scanner.scan(&to_rgb(first), first.width, first.height);
        assert_eq!(outcome.seen, 1, "one part seen before the reset");

        scanner.reset();
        let second = &frames[1];
        let outcome = scanner.scan(&to_rgb(second), second.width, second.height);
        assert_eq!(outcome.seen, 1, "the reset forgot the first part");
    }
}
