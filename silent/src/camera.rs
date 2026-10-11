//! Background camera capture, nothing more.
//!
//! One dedicated thread per [`Camera`] opens the device, pulls frames and publishes
//! the latest one behind a mutex, as RGB888 at most `MAX_FRAME_PIXELS`, the size a QR
//! scan reads. What the frames hold is the caller's business.

use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc, LazyLock, Mutex,
};
use std::{collections::HashSet, thread::JoinHandle};

use crate::{
    ffi,
    image::{box_downscale, downscale_factor, RGB_CHANNELS},
};

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
/// bwk_qr::Config::default().max_image_pixels: a larger frame is downscaled before it
/// is published, as a scan would refuse it.
const MAX_FRAME_PIXELS: usize = 1024 * 1024;

#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum CameraError {
    #[error("camera capture is only available on Linux")]
    Unsupported,
    #[error("failed to open camera {index}: {message}")]
    Open { index: u32, message: String },
    #[error("camera capture error: {0}")]
    Capture(String),
    #[error("no camera was found")]
    NoCamera,
    #[error("no camera format meets the minimum resolution and frame rate; offered: {0}")]
    NoSuitableFormat(String),
    #[error("camera {0} is already in use")]
    Busy(u32),
}

/// One captured picture. `seq` is monotonic so a reader repaints only when the frame
/// actually changed. `data` is RGB888: 3 interleaved bytes per pixel.
#[derive(Clone)]
pub struct Frame {
    pub data: Vec<u8>,
    pub width: u32,
    pub height: u32,
    pub seq: u64,
}

pub struct CameraInfo {
    pub index: u32,
    pub name: String,
}

#[derive(Default)]
struct Shared {
    frame: Option<Frame>,
    /// Why the capture thread stopped on its own, once it has.
    failure: Option<String>,
    seq: u64,
}

pub struct Camera {
    shared: Arc<Mutex<Shared>>,
    stop_flag: Arc<AtomicBool>,
    handle: Option<JoinHandle<()>>,
}

impl Camera {
    /// Only blocks to spawn the capture thread: the device opens inside it, and a
    /// failure there shows up later through `failure()`.
    #[cfg(target_os = "linux")]
    pub fn start(index: u32) -> Result<Camera, CameraError> {
        if !reserve_camera(index) {
            return Err(CameraError::Busy(index));
        }

        let shared = Arc::new(Mutex::new(Shared::default()));
        let stop_flag = Arc::new(AtomicBool::new(false));

        let thread_shared = Arc::clone(&shared);
        let thread_stop = Arc::clone(&stop_flag);

        let handle = std::thread::Builder::new()
            .name("camera-capture".to_string())
            .spawn(move || {
                let _lease = CameraLease(index);
                if let Err(e) = capture_loop(index, &thread_shared, &thread_stop) {
                    match thread_shared.lock() {
                        Ok(mut shared) => shared.failure = Some(e.to_string()),
                        Err(_) => log::error!("camera: shared state poisoned"),
                    }
                }
            })
            .map_err(|e| {
                release_camera(index);
                CameraError::Open {
                    index,
                    message: e.to_string(),
                }
            })?;

        Ok(Camera {
            shared,
            stop_flag,
            handle: Some(handle),
        })
    }

    #[cfg(not(target_os = "linux"))]
    pub fn start(_index: u32) -> Result<Camera, CameraError> {
        Err(CameraError::Unsupported)
    }

    /// Does not join. A v4l2 read has no timeout, so the capture thread can be
    /// blocked in `camera.frame()` when this is called from the GUI thread
    /// (modal close, camera switch). Dropping the join handle detaches the
    /// thread: it keeps running, notices the stop flag or a failed `frame()`
    /// on its own and releases the device.
    pub fn stop(&mut self) {
        self.stop_flag.store(true, Ordering::Relaxed);
        self.handle.take();
    }

    pub fn frame(&self) -> Option<Frame> {
        let Ok(shared) = self.shared.lock() else {
            log::error!("camera: shared state poisoned");
            return None;
        };
        shared.frame.clone()
    }

    pub fn failure(&self) -> Option<String> {
        let Ok(shared) = self.shared.lock() else {
            return Some("camera: shared state poisoned".to_string());
        };
        shared.failure.clone()
    }
}

impl Drop for Camera {
    fn drop(&mut self) {
        self.stop();
    }
}

#[cfg(target_os = "linux")]
pub fn list_cameras() -> Result<Vec<CameraInfo>, CameraError> {
    let infos =
        nokhwa::query(ApiBackend::Video4Linux).map_err(|e| CameraError::Capture(e.to_string()))?;
    if infos.is_empty() {
        return Err(CameraError::NoCamera);
    }
    let mut cameras = Vec::with_capacity(infos.len());
    for info in infos {
        let index = info
            .index()
            .as_index()
            .map_err(|e| CameraError::Capture(e.to_string()))?;
        cameras.push(CameraInfo {
            index,
            name: info.human_name(),
        });
    }
    Ok(cameras)
}

#[cfg(not(target_os = "linux"))]
pub fn list_cameras() -> Result<Vec<CameraInfo>, CameraError> {
    Err(CameraError::Unsupported)
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
    shared: &Mutex<Shared>,
    stop_flag: &AtomicBool,
) -> Result<(), CameraError> {
    let open_error = |message: String| CameraError::Open { index, message };
    let mut camera = NokhwaCamera::new(
        CameraIndex::Index(index),
        RequestedFormat::new::<RgbFormat>(RequestedFormatType::None),
    )
    .map_err(|e| open_error(e.to_string()))?;

    let offered = camera
        .compatible_camera_formats()
        .map_err(|e| CameraError::Capture(e.to_string()))?;
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
        return Err(CameraError::NoSuitableFormat(format_offered(&offered)));
    };
    log::info!(
        "camera {index}: selected {}x{}@{} {}",
        chosen.width(),
        chosen.height(),
        chosen.frame_rate(),
        chosen.format()
    );
    camera
        .set_camera_requset(RequestedFormat::new::<RgbFormat>(
            RequestedFormatType::Exact(chosen),
        ))
        .map_err(|e| open_error(e.to_string()))?;
    camera
        .open_stream()
        .map_err(|e| open_error(e.to_string()))?;

    while !stop_flag.load(Ordering::Relaxed) {
        let rgb = camera
            .frame()
            .and_then(|buffer| buffer.decode_image::<RgbFormat>())
            .map_err(|e| CameraError::Capture(e.to_string()))?;
        let width = rgb.width();
        let height = rgb.height();
        let factor = downscale_factor(width, height, MAX_FRAME_PIXELS);
        let frame = box_downscale(&rgb.into_raw(), width, height, RGB_CHANNELS, factor);

        let Ok(mut state) = shared.lock() else {
            return Err(CameraError::Capture("shared state poisoned".to_string()));
        };
        state.seq += 1;
        state.frame = Some(Frame {
            data: frame.data,
            width: frame.width,
            height: frame.height,
            seq: state.seq,
        });
    }
    Ok(())
}

// ===== Bridge =====

pub fn camera_list() -> ffi::CameraListResult {
    match list_cameras() {
        Ok(infos) => ffi::CameraListResult {
            is_ok: true,
            error: String::new(),
            names: infos.iter().map(|info| info.name.clone()).collect(),
            indexes: infos.iter().map(|info| info.index).collect(),
        },
        Err(e) => ffi::CameraListResult {
            is_ok: false,
            error: e.to_string(),
            names: Vec::new(),
            indexes: Vec::new(),
        },
    }
}

/// A running camera, or why it could not start. Dropping it stops the capture.
pub struct CameraHandle {
    inner: Result<Camera, String>,
}

impl CameraHandle {
    pub fn is_ok(&self) -> bool {
        self.inner.is_ok()
    }

    pub fn get_error(&self) -> String {
        self.inner.as_ref().err().cloned().unwrap_or_default()
    }

    /// The latest frame, is_ok false until the first one is captured.
    pub fn frame(&self) -> ffi::CameraFrameResult {
        let frame = match self.inner.as_ref() {
            Ok(camera) => camera.frame(),
            Err(e) => return failed_frame(e),
        };
        match frame {
            Some(frame) => ffi::CameraFrameResult {
                is_ok: true,
                error: String::new(),
                data: frame.data,
                width: frame.width,
                height: frame.height,
                seq: frame.seq,
            },
            None => failed_frame("no frame captured yet"),
        }
    }

    /// Why the capture stopped on its own, empty while it runs.
    pub fn failure(&self) -> String {
        match self.inner.as_ref() {
            Ok(camera) => camera.failure().unwrap_or_default(),
            Err(e) => e.clone(),
        }
    }
}

/// Spawns the capture thread for camera `index`; the device opens inside it. Call
/// from a worker thread, not the GUI thread.
pub fn camera_open(index: u32) -> Box<CameraHandle> {
    Box::new(CameraHandle {
        inner: Camera::start(index).map_err(|e| e.to_string()),
    })
}

fn failed_frame(error: &str) -> ffi::CameraFrameResult {
    ffi::CameraFrameResult {
        is_ok: false,
        error: error.to_string(),
        data: Vec::new(),
        width: 0,
        height: 0,
        seq: 0,
    }
}

#[cfg(test)]
mod tests {
    #[cfg(target_os = "linux")]
    use nokhwa::utils::{CameraFormat, FrameFormat, Resolution};

    #[cfg(target_os = "linux")]
    use crate::camera::{format_offered, select_format};

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
}
