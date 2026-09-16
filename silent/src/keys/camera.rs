//! Background camera capture and QR decode.
//!
//! One dedicated thread per [`Camera`] opens the device, pulls frames, feeds them to a
//! `bwk_qr::decoder::Decoder`, and publishes a preview plus the reassembly progress
//! behind a mutex. Decoded messages go out on the caller-supplied channel. Rust owns
//! capture and decoding; the caller (eventually the CXX bridge) only paints what it is
//! handed.

use std::collections::HashSet;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, LazyLock, Mutex};
use std::thread::JoinHandle;

use crossbeam::channel;

use crate::keys::error::Error;

#[cfg(target_os = "linux")]
use nokhwa::pixel_format::RgbFormat;
#[cfg(target_os = "linux")]
use nokhwa::utils::{
    ApiBackend, CameraFormat, CameraIndex, FrameFormat, RequestedFormat, RequestedFormatType,
};
#[cfg(target_os = "linux")]
use nokhwa::Camera as NokhwaCamera;

/// Minimum capture mode for scanning an animated QR code.
const MIN_CAPTURE_WIDTH: u32 = 1280;
const MIN_CAPTURE_HEIGHT: u32 = 720;
const MIN_CAPTURE_FPS: u32 = 10;
/// Handed to C++ for painting. Decoding still runs at capture resolution.
const PREVIEW_WIDTH: u32 = 640;
const PREVIEW_HEIGHT: u32 = 360;
/// bwk_qr::Config::default().max_image_pixels. A 1080p frame is 2073600 and is
/// refused as BadFrame, hence the downscale below.
const MAX_FRAME_PIXELS: usize = 1024 * 1024;

/// What the capture thread publishes. `seq` is monotonic so C++ repaints only
/// when the frame actually changed. `data` is RGB888: 3 interleaved bytes
/// (red, green, blue) per pixel.
#[derive(Clone)]
pub struct Preview {
    pub data: Vec<u8>,
    pub width: u32,
    pub height: u32,
    pub seq: u64,
}

#[derive(Clone)]
pub struct ScanProgress {
    pub seen: usize,
    pub total: usize,
    pub seq: u64,
}

/// Pushed on the channel the caller supplies, so this file stays free of any
/// bridge type.
pub enum CameraMessage {
    Decoded(bwk_qr::decoder::Decoded),
    /// The BBQR reassembly progress changed. Carries no data itself: the
    /// receiver reads the new values from `Camera::progress()`.
    Progress,
    Failed(String),
    Stopped,
}

pub struct CameraInfo {
    pub index: u32,
    pub name: String,
}

struct Shared {
    preview: Option<Preview>,
    progress: ScanProgress,
    seq: u64,
}

impl Default for Shared {
    fn default() -> Self {
        Shared {
            preview: None,
            progress: ScanProgress {
                seen: 0,
                total: 0,
                seq: 0,
            },
            seq: 0,
        }
    }
}

pub struct Camera {
    shared: Arc<Mutex<Shared>>,
    stop_flag: Arc<AtomicBool>,
    reset_flag: Arc<AtomicBool>,
    handle: Option<JoinHandle<()>>,
}

impl Camera {
    #[cfg(target_os = "linux")]
    pub fn start(index: u32, sender: channel::Sender<CameraMessage>) -> Result<Camera, Error> {
        if !reserve_camera(index) {
            return Err(Error::CameraBusy(index));
        }

        let shared = Arc::new(Mutex::new(Shared::default()));
        let stop_flag = Arc::new(AtomicBool::new(false));
        let reset_flag = Arc::new(AtomicBool::new(false));

        let thread_shared = Arc::clone(&shared);
        let thread_stop = Arc::clone(&stop_flag);
        let thread_reset = Arc::clone(&reset_flag);

        let handle = std::thread::Builder::new()
            .name("camera-capture".to_string())
            .spawn(move || capture_loop(index, thread_shared, thread_stop, thread_reset, sender))
            .map_err(|e| {
                release_camera(index);
                Error::CameraOpen {
                    index,
                    message: e.to_string(),
                }
            })?;

        Ok(Camera {
            shared,
            stop_flag,
            reset_flag,
            handle: Some(handle),
        })
    }

    #[cfg(not(target_os = "linux"))]
    pub fn start(_index: u32, _sender: channel::Sender<CameraMessage>) -> Result<Camera, Error> {
        Err(Error::CameraUnsupported)
    }

    /// Does not join. A v4l2 read has no timeout, so the capture thread can be
    /// blocked in `camera.frame()` when this is called from the GUI thread
    /// (modal close, camera switch). Dropping the join handle detaches the
    /// thread: it keeps running, notices the stop flag or a failed `frame()`
    /// on its own, releases the device and reports `CameraMessage::Stopped`.
    pub fn stop(&mut self) {
        self.stop_flag.store(true, Ordering::Relaxed);
        self.handle.take();
    }

    pub fn preview(&self) -> Option<Preview> {
        let Ok(shared) = self.shared.lock() else {
            log::error!("camera: shared state poisoned");
            return None;
        };
        shared.preview.clone()
    }

    pub fn progress(&self) -> ScanProgress {
        let Ok(shared) = self.shared.lock() else {
            log::error!("camera: shared state poisoned");
            return ScanProgress {
                seen: 0,
                total: 0,
                seq: 0,
            };
        };
        shared.progress.clone()
    }

    /// Drops the partial BBQR reassembly. Sets a flag the capture thread acts on,
    /// because the Decoder lives on that thread and is never shared.
    pub fn reset(&self) {
        self.reset_flag.store(true, Ordering::Relaxed);
    }
}

impl Drop for Camera {
    fn drop(&mut self) {
        self.stop();
    }
}

#[cfg(target_os = "linux")]
pub fn list_cameras() -> Result<Vec<CameraInfo>, Error> {
    let infos =
        nokhwa::query(ApiBackend::Video4Linux).map_err(|e| Error::CameraCapture(e.to_string()))?;
    if infos.is_empty() {
        return Err(Error::NoCamera);
    }
    let mut cameras = Vec::with_capacity(infos.len());
    for info in infos {
        let index = info
            .index()
            .as_index()
            .map_err(|e| Error::CameraCapture(e.to_string()))?;
        cameras.push(CameraInfo {
            index,
            name: info.human_name(),
        });
    }
    Ok(cameras)
}

#[cfg(not(target_os = "linux"))]
pub fn list_cameras() -> Result<Vec<CameraInfo>, Error> {
    Err(Error::CameraUnsupported)
}

/// Camera devices are a process-wide resource: v4l2 refuses a second open of the
/// same index, so which indexes currently have a capture thread running is tracked
/// here rather than per `Camera` instance.
#[cfg(target_os = "linux")]
static RUNNING_CAMERAS: LazyLock<Mutex<HashSet<u32>>> =
    LazyLock::new(|| Mutex::new(HashSet::new()));

/// Reserves `index` for capture. Returns false if it is already reserved.
#[cfg(target_os = "linux")]
fn reserve_camera(index: u32) -> bool {
    let Ok(mut running) = RUNNING_CAMERAS.lock() else {
        log::error!("camera: busy-set poisoned");
        return false;
    };
    running.insert(index)
}

#[cfg(target_os = "linux")]
fn release_camera(index: u32) {
    let Ok(mut running) = RUNNING_CAMERAS.lock() else {
        log::error!("camera: busy-set poisoned");
        return;
    };
    running.remove(&index);
}

/// Releases the busy-set reservation when the capture thread exits, on every
/// return path including an early failure.
#[cfg(target_os = "linux")]
struct CameraLease(u32);

#[cfg(target_os = "linux")]
impl Drop for CameraLease {
    fn drop(&mut self) {
        release_camera(self.0);
    }
}

/// Sends the failure and the stop sentinel together, since every capture-thread exit
/// on error reports both.
#[cfg(target_os = "linux")]
fn fail(sender: &channel::Sender<CameraMessage>, error: Error) {
    let _ = sender.send(CameraMessage::Failed(error.to_string()));
    let _ = sender.send(CameraMessage::Stopped);
}

/// Modes with a frame rate below `MIN_CAPTURE_FPS` or a resolution smaller than
/// `MIN_CAPTURE_WIDTH` x `MIN_CAPTURE_HEIGHT` do not qualify. MJPEG qualifies too;
/// `select_format` only uses it when no raw mode does.
#[cfg(target_os = "linux")]
const PREFERRED_FORMATS: [FrameFormat; 2] = [FrameFormat::YUYV, FrameFormat::NV12];

#[cfg(target_os = "linux")]
fn qualifies(format: &CameraFormat) -> bool {
    format.frame_rate() >= MIN_CAPTURE_FPS
        && format.width() >= MIN_CAPTURE_WIDTH
        && format.height() >= MIN_CAPTURE_HEIGHT
        && (PREFERRED_FORMATS.contains(&format.format()) || format.format() == FrameFormat::MJPEG)
}

/// Picks the capture format for QR scanning: raw YUYV/NV12 over MJPEG, then the
/// lowest qualifying resolution, then the highest frame rate. Pure over the modes
/// nokhwa reports, so it is unit-tested without a device.
#[cfg(target_os = "linux")]
fn select_format(offered: &[CameraFormat]) -> Option<CameraFormat> {
    let candidates: Vec<CameraFormat> = offered.iter().copied().filter(qualifies).collect();
    let preferred: Vec<CameraFormat> = candidates
        .iter()
        .copied()
        .filter(|format| PREFERRED_FORMATS.contains(&format.format()))
        .collect();
    let pool = if preferred.is_empty() {
        &candidates
    } else {
        &preferred
    };
    pool.iter().copied().min_by_key(|format| {
        (
            u64::from(format.width()) * u64::from(format.height()),
            u32::MAX - format.frame_rate(),
        )
    })
}

/// Lists offered modes as `WxH@fps FORMAT`, for the error when none qualifies.
#[cfg(target_os = "linux")]
fn format_offered(offered: &[CameraFormat]) -> String {
    offered
        .iter()
        .map(|format| {
            format!(
                "{}x{}@{} {}",
                format.width(),
                format.height(),
                format.frame_rate(),
                format.format()
            )
        })
        .collect::<Vec<String>>()
        .join(", ")
}

#[cfg(target_os = "linux")]
fn capture_loop(
    index: u32,
    shared: Arc<Mutex<Shared>>,
    stop_flag: Arc<AtomicBool>,
    reset_flag: Arc<AtomicBool>,
    sender: channel::Sender<CameraMessage>,
) {
    let _lease = CameraLease(index);

    let mut camera = match NokhwaCamera::new(
        CameraIndex::Index(index),
        RequestedFormat::new::<RgbFormat>(RequestedFormatType::None),
    ) {
        Ok(camera) => camera,
        Err(e) => {
            fail(
                &sender,
                Error::CameraOpen {
                    index,
                    message: e.to_string(),
                },
            );
            return;
        }
    };

    let offered = match camera.compatible_camera_formats() {
        Ok(formats) => formats,
        Err(e) => {
            fail(&sender, Error::CameraCapture(e.to_string()));
            return;
        }
    };
    for mode in &offered {
        log::debug!(
            "camera {index}: offered {}x{}@{} {}",
            mode.width(),
            mode.height(),
            mode.frame_rate(),
            mode.format()
        );
    }

    let Some(chosen) = select_format(&offered) else {
        fail(
            &sender,
            Error::CameraNoSuitableFormat(format_offered(&offered)),
        );
        return;
    };
    log::info!(
        "camera {index}: selected {}x{}@{} {}",
        chosen.width(),
        chosen.height(),
        chosen.frame_rate(),
        chosen.format()
    );

    if let Err(e) = camera.set_camera_requset(RequestedFormat::new::<RgbFormat>(
        RequestedFormatType::Exact(chosen),
    )) {
        fail(
            &sender,
            Error::CameraOpen {
                index,
                message: e.to_string(),
            },
        );
        return;
    }

    if let Err(e) = camera.open_stream() {
        fail(
            &sender,
            Error::CameraOpen {
                index,
                message: e.to_string(),
            },
        );
        return;
    }

    let mut decoder = match bwk_qr::decoder::Decoder::new(bwk_qr::config::Config::default()) {
        Ok(decoder) => decoder,
        Err(e) => {
            fail(&sender, Error::from(e));
            return;
        }
    };
    // Matches `Shared::default().progress`, so the first frame does not push a
    // spurious progress event when nothing has been scanned yet.
    let mut last_progress = (0usize, 0usize);

    while !stop_flag.load(Ordering::Relaxed) {
        if reset_flag.swap(false, Ordering::Relaxed) {
            decoder.reset();
        }

        let buffer = match camera.frame() {
            Ok(buffer) => buffer,
            Err(e) => {
                fail(&sender, Error::CameraCapture(e.to_string()));
                return;
            }
        };
        let rgb = match buffer.decode_image::<RgbFormat>() {
            Ok(rgb) => rgb,
            Err(e) => {
                fail(&sender, Error::CameraCapture(e.to_string()));
                return;
            }
        };

        let width = rgb.width();
        let height = rgb.height();
        let rgb_data = rgb.into_raw();
        let image = bwk_qr::image::Image {
            data: rgb_to_luma(&rgb_data, width, height),
            width,
            height,
        };

        let factor = downscale_factor(width, height);
        // a camera frame routinely holds unreadable candidates
        let process_result = if factor > 1 {
            decoder.process(&box_downscale(&image, factor))
        } else {
            decoder.process(&image)
        };
        match process_result {
            Ok(messages) => {
                for message in messages {
                    if sender.send(CameraMessage::Decoded(message)).is_err() {
                        return;
                    }
                }
            }
            Err(e) => log::debug!("camera: bad frame: {e}"),
        }

        let preview = rgb_box_downscale(&rgb_data, width, height, preview_factor(width, height));
        let progress = decoder.progress();
        let (seen, total) = match progress {
            Some(p) => (p.seen, p.total),
            None => (0, 0),
        };

        {
            let Ok(mut state) = shared.lock() else {
                log::error!("camera: shared state poisoned");
                return;
            };
            state.seq += 1;
            let seq = state.seq;
            state.preview = Some(Preview {
                data: preview.data,
                width: preview.width,
                height: preview.height,
                seq,
            });
            state.progress = ScanProgress { seen, total, seq };
        }

        if (seen, total) != last_progress {
            last_progress = (seen, total);
            if sender.send(CameraMessage::Progress).is_err() {
                return;
            }
        }
    }

    let _ = sender.send(CameraMessage::Stopped);
}

/// Integer luma weights (ITU-R BT.601, times 1000) so grey decoding stays exact
/// integer math on the RGB frame nokhwa hands back.
const LUMA_RED: u32 = 299;
const LUMA_GREEN: u32 = 587;
const LUMA_BLUE: u32 = 114;

/// Converts an interleaved RGB888 buffer to one byte per pixel for `bwk_qr`, which
/// only accepts grayscale.
fn rgb_to_luma(rgb: &[u8], width: u32, height: u32) -> Vec<u8> {
    let mut data = Vec::with_capacity((width * height) as usize);
    for pixel in rgb.chunks_exact(3) {
        let luma = (u32::from(pixel[0]) * LUMA_RED
            + u32::from(pixel[1]) * LUMA_GREEN
            + u32::from(pixel[2]) * LUMA_BLUE)
            / 1000;
        data.push(luma as u8);
    }
    data
}

/// Averages one channel over a `factor` x `factor` block starting at pixel
/// (`origin_x`, `origin_y`). Shared by the grayscale and RGB box downscales so the
/// averaging logic exists once. Out-of-bounds reads (none expected) contribute 0
/// rather than panicking.
fn box_downscale_channel(
    data: &[u8],
    width: u32,
    origin_x: u32,
    origin_y: u32,
    factor: u32,
    channels: u32,
    channel: u32,
) -> u8 {
    let mut sum: u32 = 0;
    for dy in 0..factor {
        let row_start = ((origin_y + dy) * width * channels) as usize;
        for dx in 0..factor {
            let index = row_start + ((origin_x + dx) * channels + channel) as usize;
            sum += u32::from(data.get(index).copied().unwrap_or(0));
        }
    }
    (sum / (factor * factor)) as u8
}

/// Integer box downscale by `factor`: each output pixel is the average of a
/// factor x factor block. Integer only, so no interpolation blurs a QR edge.
fn box_downscale(image: &bwk_qr::image::Image, factor: u32) -> bwk_qr::image::Image {
    if factor <= 1 {
        return image.clone();
    }
    let out_width = image.width / factor;
    let out_height = image.height / factor;
    let mut data = Vec::with_capacity((out_width * out_height) as usize);
    for out_y in 0..out_height {
        for out_x in 0..out_width {
            data.push(box_downscale_channel(
                &image.data,
                image.width,
                out_x * factor,
                out_y * factor,
                factor,
                1,
                0,
            ));
        }
    }
    bwk_qr::image::Image {
        data,
        width: out_width,
        height: out_height,
    }
}

/// A downscaled RGB888 preview frame: `data.len() == width * height * 3`.
struct RgbImage {
    data: Vec<u8>,
    width: u32,
    height: u32,
}

/// RGB888 counterpart to `box_downscale`: each output pixel is the block average
/// of red, green and blue independently, via the shared averaging helper.
fn rgb_box_downscale(data: &[u8], width: u32, height: u32, factor: u32) -> RgbImage {
    if factor <= 1 {
        return RgbImage {
            data: data.to_vec(),
            width,
            height,
        };
    }
    let out_width = width / factor;
    let out_height = height / factor;
    let mut out = Vec::with_capacity((out_width * out_height * 3) as usize);
    for out_y in 0..out_height {
        for out_x in 0..out_width {
            for channel in 0..3 {
                out.push(box_downscale_channel(
                    data,
                    width,
                    out_x * factor,
                    out_y * factor,
                    factor,
                    3,
                    channel,
                ));
            }
        }
    }
    RgbImage {
        data: out,
        width: out_width,
        height: out_height,
    }
}

/// Smallest integer factor that brings the real capture frame within
/// `PREVIEW_WIDTH` x `PREVIEW_HEIGHT`. The capture size is no longer fixed, so this
/// is computed per frame rather than assumed.
fn preview_factor(width: u32, height: u32) -> u32 {
    width
        .div_ceil(PREVIEW_WIDTH)
        .max(height.div_ceil(PREVIEW_HEIGHT))
        .max(1)
}

/// Smallest factor >= 1 that brings the frame under `MAX_FRAME_PIXELS`.
fn downscale_factor(width: u32, height: u32) -> u32 {
    let mut factor: u32 = 1;
    while (width / factor) as usize * (height / factor) as usize > MAX_FRAME_PIXELS {
        factor += 1;
    }
    factor
}

#[cfg(test)]
mod tests {
    use crate::keys::camera::{rgb_box_downscale, rgb_to_luma};

    #[cfg(target_os = "linux")]
    use nokhwa::utils::{CameraFormat, FrameFormat, Resolution};

    #[cfg(target_os = "linux")]
    use crate::keys::camera::{format_offered, select_format};

    #[cfg(target_os = "linux")]
    fn mode(width: u32, height: u32, fps: u32, format: FrameFormat) -> CameraFormat {
        CameraFormat::new(Resolution::new(width, height), format, fps)
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn select_format_prefers_raw_over_mjpeg_at_equal_resolution() {
        let offered = [
            mode(1280, 720, 10, FrameFormat::YUYV),
            mode(1280, 720, 30, FrameFormat::MJPEG),
        ];
        let chosen = select_format(&offered).expect("a format qualifies");
        assert_eq!(chosen.format(), FrameFormat::YUYV);
        assert_eq!(chosen.resolution(), Resolution::new(1280, 720));
        assert_eq!(chosen.frame_rate(), 10);
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn select_format_falls_back_to_mjpeg_lowest_resolution_highest_fps() {
        let offered = [
            mode(1280, 720, 5, FrameFormat::YUYV),
            mode(1920, 1080, 30, FrameFormat::MJPEG),
            mode(1280, 720, 30, FrameFormat::MJPEG),
        ];
        let chosen = select_format(&offered).expect("a format qualifies");
        assert_eq!(chosen.format(), FrameFormat::MJPEG);
        assert_eq!(chosen.resolution(), Resolution::new(1280, 720));
        assert_eq!(chosen.frame_rate(), 30);
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn select_format_rejects_modes_below_the_minimum() {
        let offered = [mode(640, 480, 30, FrameFormat::MJPEG)];
        assert!(select_format(&offered).is_none());
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn format_offered_lists_every_mode() {
        let offered = [mode(640, 480, 30, FrameFormat::MJPEG)];
        assert_eq!(format_offered(&offered), "640x480@30 MJPEG");
    }

    #[test]
    fn rgb_to_luma_uses_known_weights() {
        let rgb = [255u8, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255];
        let luma = rgb_to_luma(&rgb, 2, 2);
        assert_eq!(luma, vec![76, 149, 29, 255]);
    }

    #[test]
    fn rgb_box_downscale_averages_a_4x2_block_by_2() {
        let width = 4;
        let height = 2;
        let mut data = Vec::with_capacity((width * height * 3) as usize);
        for y in 0..height {
            for x in 0..width {
                let value = (y * width + x) as u8;
                data.extend_from_slice(&[value, value, value]);
            }
        }
        let downscaled = rgb_box_downscale(&data, width, height, 2);
        assert_eq!(downscaled.width, 2);
        assert_eq!(downscaled.height, 1);
        assert_eq!(downscaled.data, vec![2, 2, 2, 4, 4, 4]);
    }
}
