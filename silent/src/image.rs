//! Raw raster helpers shared by the camera and the QR scanner.

/// Luma weights (ITU-R BT.601, times 1000), so the conversion is exact integer math.
const LUMA_RED: u32 = 299;
const LUMA_GREEN: u32 = 587;
const LUMA_BLUE: u32 = 114;
pub const RGB_CHANNELS: u32 = 3;

/// Interleaved pixels, `channels` bytes each, row-major.
pub struct Raster {
    pub data: Vec<u8>,
    pub width: u32,
    pub height: u32,
}

/// RGB888 to one byte per pixel.
pub fn rgb_to_luma(rgb: &[u8]) -> Vec<u8> {
    rgb.chunks_exact(RGB_CHANNELS as usize)
        .map(|pixel| {
            ((u32::from(pixel[0]) * LUMA_RED
                + u32::from(pixel[1]) * LUMA_GREEN
                + u32::from(pixel[2]) * LUMA_BLUE)
                / 1000) as u8
        })
        .collect()
}

/// Smallest factor >= 1 that brings `width` x `height` within `max_pixels`.
pub fn downscale_factor(width: u32, height: u32, max_pixels: usize) -> u32 {
    let mut factor: u32 = 1;
    while (width / factor) as usize * (height / factor) as usize > max_pixels {
        factor += 1;
    }
    factor
}

/// Integer box downscale by `factor`: each output pixel averages a factor x factor
/// block, channel by channel, so no interpolation blurs a QR module edge.
pub fn box_downscale(data: &[u8], width: u32, height: u32, channels: u32, factor: u32) -> Raster {
    if factor <= 1 {
        return Raster {
            data: data.to_vec(),
            width,
            height,
        };
    }
    let out_width = width / factor;
    let out_height = height / factor;
    let mut out = Vec::with_capacity((out_width * out_height * channels) as usize);
    for y in 0..out_height {
        for x in 0..out_width {
            for channel in 0..channels {
                let mut sum: u32 = 0;
                for dy in 0..factor {
                    let row = ((y * factor + dy) * width * channels) as usize;
                    for dx in 0..factor {
                        let index = row + ((x * factor + dx) * channels + channel) as usize;
                        sum += u32::from(data.get(index).copied().unwrap_or(0));
                    }
                }
                out.push((sum / (factor * factor)) as u8);
            }
        }
    }
    Raster {
        data: out,
        width: out_width,
        height: out_height,
    }
}

#[cfg(test)]
mod tests {
    use crate::image::{box_downscale, downscale_factor, rgb_to_luma, RGB_CHANNELS};

    #[test]
    fn rgb_to_luma_uses_known_weights() {
        let rgb = [255u8, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255];
        assert_eq!(rgb_to_luma(&rgb), vec![76, 149, 29, 255]);
    }

    #[test]
    fn box_downscale_averages_each_rgb_channel_of_a_4x2_block_by_2() {
        let mut data = Vec::new();
        for value in 0..8u8 {
            data.extend_from_slice(&[value, value, value]);
        }
        let downscaled = box_downscale(&data, 4, 2, RGB_CHANNELS, 2);
        assert_eq!((downscaled.width, downscaled.height), (2, 1));
        assert_eq!(downscaled.data, vec![2, 2, 2, 4, 4, 4]);
    }

    #[test]
    fn box_downscale_averages_a_grayscale_4x2_block_by_2() {
        let data: Vec<u8> = (0..8).collect();
        let downscaled = box_downscale(&data, 4, 2, 1, 2);
        assert_eq!((downscaled.width, downscaled.height), (2, 1));
        assert_eq!(downscaled.data, vec![2, 4]);
    }

    #[test]
    fn downscale_factor_is_the_smallest_that_fits() {
        assert_eq!(downscale_factor(1280, 720, 1024 * 1024), 1);
        assert_eq!(downscale_factor(1920, 1080, 1024 * 1024), 2);
    }
}
