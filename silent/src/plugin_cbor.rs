use crate::plugin_admission::MAX_METADATA_BYTES;
use serde::de::{DeserializeSeed, Deserializer, Error, MapAccess, SeqAccess, Visitor};
use serde_cbor::Value;
use std::{collections::BTreeMap, fmt};

const MAX_DEPTH: usize = 32;
const MAX_ITEMS: usize = 16_384;

pub(crate) struct MetadataCbor {
    pub iid: String,
    pub consumed: usize,
}

pub(crate) fn read(data: &[u8]) -> Result<MetadataCbor, String> {
    if data.len() > MAX_METADATA_BYTES {
        return Err("CBOR byte budget exceeded".into());
    }
    let mut budget = Budget {
        items: MAX_ITEMS,
        bytes: MAX_METADATA_BYTES,
    };
    let mut decoder = serde_cbor::Deserializer::from_slice(data);
    let value = StrictValue {
        budget: &mut budget,
        depth: 0,
    }
    .deserialize(&mut decoder)
    .map_err(|e| e.to_string())?;
    let Value::Map(mut root) = value else {
        return Err("Qt metadata root is not an untagged map".into());
    };
    let Some(Value::Text(iid)) = root.remove(&Value::Integer(2)) else {
        return Err("missing or non-string Qt metadata root IID".into());
    };
    Ok(MetadataCbor {
        iid,
        consumed: decoder.byte_offset(),
    })
}

struct Budget {
    items: usize,
    bytes: usize,
}

impl Budget {
    fn bytes<E: Error>(&mut self, count: usize) -> Result<(), E> {
        self.bytes = self
            .bytes
            .checked_sub(count)
            .ok_or_else(|| E::custom("CBOR byte budget exceeded"))?;
        Ok(())
    }
}

struct StrictValue<'a> {
    budget: &'a mut Budget,
    depth: usize,
}

impl<'de> DeserializeSeed<'de> for StrictValue<'_> {
    type Value = Value;

    fn deserialize<D: Deserializer<'de>>(self, deserializer: D) -> Result<Value, D::Error> {
        if self.depth > MAX_DEPTH {
            return Err(D::Error::custom("CBOR nesting depth exceeded"));
        }
        self.budget.items = self
            .budget
            .items
            .checked_sub(1)
            .ok_or_else(|| D::Error::custom("CBOR item budget exceeded"))?;
        deserializer.deserialize_any(self)
    }
}

impl<'de> Visitor<'de> for StrictValue<'_> {
    type Value = Value;

    fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        formatter.write_str("bounded CBOR with unique map keys at every depth")
    }

    fn visit_bool<E: Error>(self, value: bool) -> Result<Value, E> {
        Ok(Value::Bool(value))
    }
    fn visit_i64<E: Error>(self, value: i64) -> Result<Value, E> {
        Ok(Value::Integer(value.into()))
    }
    fn visit_u64<E: Error>(self, value: u64) -> Result<Value, E> {
        Ok(Value::Integer(value.into()))
    }
    fn visit_f64<E: Error>(self, value: f64) -> Result<Value, E> {
        Ok(Value::Float(value))
    }
    fn visit_unit<E: Error>(self) -> Result<Value, E> {
        Ok(Value::Null)
    }

    fn visit_str<E: Error>(self, value: &str) -> Result<Value, E> {
        self.budget.bytes::<E>(value.len())?;
        Ok(Value::Text(value.to_owned()))
    }

    fn visit_string<E: Error>(self, value: String) -> Result<Value, E> {
        self.budget.bytes::<E>(value.len())?;
        Ok(Value::Text(value))
    }

    fn visit_bytes<E: Error>(self, value: &[u8]) -> Result<Value, E> {
        self.budget.bytes::<E>(value.len())?;
        Ok(Value::Bytes(value.to_vec()))
    }

    fn visit_byte_buf<E: Error>(self, value: Vec<u8>) -> Result<Value, E> {
        self.budget.bytes::<E>(value.len())?;
        Ok(Value::Bytes(value))
    }

    fn visit_seq<S: SeqAccess<'de>>(self, mut sequence: S) -> Result<Value, S::Error> {
        if sequence
            .size_hint()
            .is_some_and(|size| size > self.budget.items)
        {
            return Err(S::Error::custom("CBOR array exceeds item budget"));
        }
        let mut values = Vec::new();
        while let Some(value) = sequence.next_element_seed(StrictValue {
            budget: self.budget,
            depth: self.depth + 1,
        })? {
            values.push(value);
        }
        Ok(Value::Array(values))
    }

    fn visit_map<M: MapAccess<'de>>(self, mut map: M) -> Result<Value, M::Error> {
        if map
            .size_hint()
            .is_some_and(|size| size > self.budget.items / 2)
        {
            return Err(M::Error::custom("CBOR map exceeds item budget"));
        }
        let mut values = BTreeMap::new();
        while let Some(key) = map.next_key_seed(StrictValue {
            budget: self.budget,
            depth: self.depth + 1,
        })? {
            if values.contains_key(&key) {
                return Err(M::Error::custom("duplicate CBOR map key"));
            }
            let value = map.next_value_seed(StrictValue {
                budget: self.budget,
                depth: self.depth + 1,
            })?;
            values.insert(key, value);
        }
        Ok(Value::Map(values))
    }

    fn visit_newtype_struct<D: Deserializer<'de>>(
        self,
        deserializer: D,
    ) -> Result<Value, D::Error> {
        let tag = serde_cbor::tags::current_cbor_tag()
            .ok_or_else(|| D::Error::custom("missing CBOR tag"))?;
        let value = StrictValue {
            budget: self.budget,
            depth: self.depth + 1,
        }
        .deserialize(deserializer)?;
        Ok(Value::Tag(tag, Box::new(value)))
    }
}
