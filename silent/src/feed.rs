use std::collections::{BTreeMap, BTreeSet};

use crate::ffi::{FeedPrice, FeedQuote, FeedSelection, FeedSourceState, RustFeePreferences};

impl RustFeePreferences {
    pub(crate) fn is_valid(&self) -> bool {
        self.mode <= 2
    }
}

#[derive(Default)]
struct Source {
    currencies: BTreeSet<String>,
    rates: BTreeMap<String, f64>,
    has_default: bool,
    has_smart: bool,
    default: Option<f64>,
    smart: [Option<f64>; 3],
}

#[derive(Default)]
pub struct FeedState {
    sources: BTreeMap<String, Source>,
    selected_source: String,
    selected_currency: String,
    fees: RustFeePreferences,
}

pub fn new_feed_state() -> Box<FeedState> {
    Box::default()
}

pub(crate) fn currency(text: &str) -> String {
    text.trim().to_uppercase()
}
fn valid(value: f64) -> Option<f64> {
    (value.is_finite() && value > 0.0).then_some(value)
}
fn quote(value: Option<f64>) -> FeedQuote {
    FeedQuote {
        available: value.is_some(),
        value: value.unwrap_or(0.0),
    }
}

impl FeedState {
    pub fn add_feed(&mut self, id: &str) {
        self.sources.entry(id.into()).or_default();
    }
    pub fn remove_feed(&mut self, id: &str) {
        self.sources.remove(id);
    }
    pub fn select_price(&mut self, source: String, selected_currency: String) {
        self.selected_source = source;
        self.selected_currency = currency(&selected_currency);
    }
    pub fn set_currencies(&mut self, id: &str, offered: Vec<String>) {
        let Some(source) = self.sources.get_mut(id) else {
            return;
        };
        source.currencies = offered
            .iter()
            .map(|text| currency(text))
            .filter(|text| !text.is_empty())
            .collect();
        source
            .rates
            .retain(|code, _| source.currencies.contains(code));
    }
    pub fn set_price_quote(&mut self, id: &str, code: &str, value: f64) {
        let Some(source) = self.sources.get_mut(id) else {
            return;
        };
        let code = currency(code);
        if !source.currencies.contains(&code) {
            return;
        }
        match valid(value) {
            Some(value) => {
                source.rates.insert(code, value);
            }
            None => {
                source.rates.remove(&code);
            }
        }
    }
    pub fn feed_source(&self, id: &str) -> FeedSourceState {
        match self.sources.get(id) {
            Some(source) => FeedSourceState {
                currencies: source.currencies.iter().cloned().collect(),
                has_default: source.has_default,
                has_smart: source.has_smart,
            },
            None => FeedSourceState::default(),
        }
    }
    pub fn price_selection(&self) -> FeedPrice {
        let value = self
            .sources
            .get(&self.selected_source)
            .and_then(|source| source.rates.get(&self.selected_currency))
            .copied();
        FeedPrice {
            source: self.selected_source.clone(),
            currency: self.selected_currency.clone(),
            quote: quote(value),
        }
    }
    pub fn select_fees(&mut self, preferences: RustFeePreferences) -> bool {
        if !preferences.is_valid() {
            return false;
        }
        self.fees = preferences;
        true
    }
    pub fn set_fee_capabilities(&mut self, id: &str, has_default: bool, has_smart: bool) {
        let Some(source) = self.sources.get_mut(id) else {
            return;
        };
        source.has_default = has_default;
        source.has_smart = has_smart;
        if !has_default {
            source.default = None;
        }
        if !has_smart {
            source.smart = [None; 3];
        }
    }
    pub fn set_default_fee(&mut self, id: &str, value: f64) {
        if let Some(source) = self.sources.get_mut(id).filter(|source| source.has_default) {
            source.default = valid(value);
        }
    }
    pub fn set_smart_fees(&mut self, id: &str, low: f64, medium: f64, fast: f64) {
        if let Some(source) = self.sources.get_mut(id).filter(|source| source.has_smart) {
            source.smart = [valid(low), valid(medium), valid(fast)];
        }
    }
    pub fn fee_selection(&self) -> FeedSelection {
        let source = self.sources.get(&self.fees.source);
        let default = source.and_then(|source| source.default);
        let smart = source.map(|source| source.smart).unwrap_or([None; 3]);
        FeedSelection {
            preferences: self.fees.clone(),
            default_rate: quote(default),
            low: quote(smart[0]),
            medium: quote(smart[1]),
            fast: quote(smart[2]),
        }
    }
}
