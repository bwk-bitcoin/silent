use crate::plugin_admission::native::{NativeImage, MAX_EXPORT_BYTES, MAX_SYMBOLS};
use object::{Object, ObjectSymbol};

#[derive(Default)]
pub(crate) struct ElfLookup {
    pub gnu: Option<u64>,
    pub sysv: Option<u64>,
    pub versions: Option<u64>,
}

impl ElfLookup {
    pub(crate) fn validate(
        &self,
        image: &NativeImage,
        bytes: &[u8],
        file: &object::File<'_>,
        index: usize,
        name: &[u8],
    ) -> Result<(), String> {
        let reachable = match (self.gnu, self.sysv) {
            (Some(address), _) => {
                let symbol_count = match file {
                    object::File::Elf32(file) => file.elf_dynamic_symbol_table().len(),
                    object::File::Elf64(file) => file.elf_dynamic_symbol_table().len(),
                    _ => return Err("GNU hash requires an ELF image".into()),
                };
                let word_size = if file.is_64() { 8 } else { 4 };
                if address % word_size != 0 {
                    return Err("unaligned ELF GNU export hash table".into());
                }
                let table =
                    GnuHash::parse(image.data_at(bytes, address)?, file.is_64(), symbol_count)?;
                table.validate_symbols(file)?;
                table.lookup(index, name)?
            }
            (None, Some(address)) => sysv_lookup(image.data_at(bytes, address)?, index, name)?,
            _ => return Err("missing ELF runtime export hash table".into()),
        };
        if !reachable {
            return Err("Qt entry is not reachable through the ELF runtime export hash".into());
        }
        if let Some(address) = self.versions {
            let table = image.data_at(bytes, address)?;
            let value = table
                .get(index * 2..index * 2 + 2)
                .ok_or("Qt entry version outside ELF table")?;
            if u16::from_le_bytes([value[0], value[1]]) != 1 {
                return Err("versioned or local ELF Qt entry exports are unsupported".into());
            }
        }
        Ok(())
    }
}

fn word(data: &[u8], offset: usize) -> Result<u32, String> {
    let end = offset
        .checked_add(4)
        .ok_or("ELF export hash offset overflow")?;
    let value = data
        .get(offset..end)
        .ok_or("ELF export hash outside mapped table")?;
    Ok(u32::from_le_bytes([value[0], value[1], value[2], value[3]]))
}

struct GnuHash<'a> {
    bloom: &'a [u8],
    buckets: &'a [u8],
    chains: &'a [u8],
    symbol_offset: usize,
    symbol_count: usize,
    word_size: usize,
    shift: u32,
}

impl<'a> GnuHash<'a> {
    fn parse(data: &'a [u8], is_64: bool, symbol_count: usize) -> Result<Self, String> {
        let bucket_count = word(data, 0)? as usize;
        let symbol_offset = word(data, 4)? as usize;
        let bloom_size = word(data, 8)? as usize;
        let shift = word(data, 12)?;
        // GNU hashes are 32-bit even when the bloom words are 64-bit.
        if bucket_count == 0
            || bucket_count > MAX_SYMBOLS
            || !bloom_size.is_power_of_two()
            || bloom_size > MAX_SYMBOLS
            || symbol_offset == 0
            || symbol_offset > symbol_count
            || symbol_count > MAX_SYMBOLS
            || shift >= u32::BITS
        {
            return Err("invalid ELF GNU export hash bounds".into());
        }
        let word_size = if is_64 { 8 } else { 4 };
        let bucket_offset = 16 + bloom_size * word_size;
        let chain_offset = bucket_offset + bucket_count * 4;
        let end = chain_offset + (symbol_count - symbol_offset) * 4;
        let table = Self {
            bloom: data
                .get(16..bucket_offset)
                .ok_or("truncated ELF GNU export hash bloom")?,
            buckets: data
                .get(bucket_offset..chain_offset)
                .ok_or("truncated ELF GNU export hash buckets")?,
            chains: data
                .get(chain_offset..end)
                .ok_or("truncated ELF GNU export hash chains")?,
            symbol_offset,
            symbol_count,
            word_size,
            shift,
        };
        for offset in (0..table.buckets.len()).step_by(4) {
            let start = word(table.buckets, offset)? as usize;
            if start != 0 && !(symbol_offset..symbol_count).contains(&start) {
                return Err("ELF GNU export hash bucket outside dynamic symbols".into());
            }
        }
        Ok(table)
    }

    fn bloom_contains(&self, hash: u32) -> Result<bool, String> {
        let bits = self.word_size * 8;
        let count = self.bloom.len() / self.word_size;
        let offset = ((hash as usize / bits) & (count - 1)) * self.word_size;
        let mut bloom = u64::from(word(self.bloom, offset)?);
        if self.word_size == 8 {
            bloom |= u64::from(word(self.bloom, offset + 4)?) << 32;
        }
        let mask =
            (1_u64 << (hash as usize % bits)) | (1_u64 << ((hash >> self.shift) as usize % bits));
        Ok(bloom & mask == mask)
    }

    fn validate_symbols(&self, file: &object::File<'_>) -> Result<(), String> {
        let mut starts = vec![0; self.buckets.len() / 4];
        let mut active_bucket = None;
        let mut next = self.symbol_offset;
        let mut name_bytes = 0;
        for symbol in file.dynamic_symbols() {
            let index = symbol.index().0;
            if index < self.symbol_offset {
                continue;
            }
            if index != next || index >= self.symbol_count {
                return Err("ELF GNU export hash symbol bounds disagree with dynamic table".into());
            }
            let name = symbol.name_bytes().map_err(|e| e.to_string())?;
            name_bytes += name.len();
            if name_bytes > MAX_EXPORT_BYTES {
                return Err("ELF GNU export hash names exceed admission byte limit".into());
            }
            let hash = gnu_hash(name);
            let entry = word(self.chains, (index - self.symbol_offset) * 4)?;
            let bucket = hash as usize % starts.len();
            if active_bucket.is_none() {
                if starts[bucket] != 0 {
                    return Err("split ELF GNU export hash bucket chain".into());
                }
                starts[bucket] = index;
                active_bucket = Some(bucket);
            }
            if active_bucket != Some(bucket) || entry & !1 != hash & !1 {
                return Err("incongruent ELF GNU export hash bucket or chain hash".into());
            }
            if !self.bloom_contains(hash)? {
                return Err("invalid ELF GNU runtime export hash bloom filter".into());
            }
            if entry & 1 != 0 {
                active_bucket = None;
            }
            next += 1;
        }
        if next != self.symbol_count || active_bucket.is_some() {
            return Err("unterminated ELF GNU export hash chain".into());
        }
        for (bucket, start) in starts.into_iter().enumerate() {
            if word(self.buckets, bucket * 4)? as usize != start {
                return Err("incongruent ELF GNU export hash bucket start".into());
            }
        }
        Ok(())
    }

    fn lookup(&self, index: usize, name: &[u8]) -> Result<bool, String> {
        let hash = gnu_hash(name);
        if !self.bloom_contains(hash)? {
            return Ok(false);
        }
        let mut current =
            word(self.buckets, (hash as usize % (self.buckets.len() / 4)) * 4)? as usize;
        if current == 0 {
            return Ok(false);
        }
        while current < self.symbol_count {
            let entry = word(self.chains, (current - self.symbol_offset) * 4)?;
            if current == index && entry & !1 == hash & !1 {
                return Ok(true);
            }
            if entry & 1 != 0 {
                return Ok(false);
            }
            current += 1;
        }
        Err("unterminated ELF GNU export hash chain".into())
    }
}

fn gnu_hash(name: &[u8]) -> u32 {
    name.iter().fold(5381_u32, |hash, byte| {
        hash.wrapping_mul(33).wrapping_add(u32::from(*byte))
    })
}

fn sysv_lookup(data: &[u8], index: usize, name: &[u8]) -> Result<bool, String> {
    let buckets = word(data, 0)? as usize;
    let chains = word(data, 4)? as usize;
    if buckets == 0 || buckets > MAX_SYMBOLS || chains == 0 || chains > MAX_SYMBOLS {
        return Err("invalid ELF SysV export hash bounds".into());
    }
    let hash = name.iter().fold(0_u32, |hash, byte| {
        let value = hash.wrapping_shl(4).wrapping_add(u32::from(*byte));
        let high = value & 0xf000_0000;
        (value ^ (high >> 24)) & !high
    });
    let mut current = word(data, 8 + (hash as usize % buckets) * 4)? as usize;
    for _ in 0..chains {
        if current == 0 {
            return Ok(false);
        }
        if current >= chains {
            return Err("ELF SysV export chain index outside table".into());
        }
        if current == index {
            return Ok(true);
        }
        current = word(data, 8 + buckets * 4 + current * 4)? as usize;
    }
    Err("cyclic ELF SysV export hash chain".into())
}

#[cfg(test)]
mod tests {
    use crate::plugin_admission::native::elf_lookup::{word, GnuHash};

    #[test]
    fn gnu_hash_truncation_returns_errors_without_unwinding() {
        for is_64 in [false, true] {
            let mut words = vec![1_u32, 1, 1, 6, u32::MAX];
            if is_64 {
                words.push(u32::MAX);
            }
            words.extend([1, 0, 1]);
            let valid: Vec<u8> = words.into_iter().flat_map(u32::to_le_bytes).collect();
            assert!(
                GnuHash::parse(&valid, is_64, 3).is_ok(),
                "complete GNU layout is valid for word size {is_64}"
            );
            for end in 0..valid.len() {
                let outcome = std::panic::catch_unwind(|| GnuHash::parse(&valid[..end], is_64, 3));
                assert!(
                    outcome
                        .expect("truncated GNU table must not unwind")
                        .is_err(),
                    "truncated GNU table at byte {end} must reject for word size {is_64}"
                );
            }
            for shift in [32, 63, 64, u32::MAX] {
                let mut invalid = valid.clone();
                invalid[12..16].copy_from_slice(&shift.to_le_bytes());
                assert!(
                    GnuHash::parse(&invalid, is_64, 3).is_err(),
                    "32-bit GNU hash shift {shift} is invalid on either bloom word size"
                );
            }
            for shift in [0_u32, 31] {
                let mut boundary = valid.clone();
                boundary[12..16].copy_from_slice(&shift.to_le_bytes());
                assert!(
                    GnuHash::parse(&boundary, is_64, 3).is_ok(),
                    "GNU hash shift {shift} is valid on either bloom word size"
                );
            }
            assert!(
                GnuHash::parse(&valid, is_64, usize::MAX).is_err(),
                "excessive symbol count must reject before offset arithmetic"
            );
        }
        assert!(
            word(&[], usize::MAX).is_err(),
            "word offset overflow must return an error"
        );
    }
}
