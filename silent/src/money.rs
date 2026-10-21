use crate::{
    account::{fee_rate_msats, MSATS_PER_SAT},
    ffi::{AmountParseResult, FeeParseResult, SendAmounts},
};

pub fn send_amounts(inputs: &[u64], recipients: &[u64], fee: u64) -> SendAmounts {
    let sum = |values: &[u64]| {
        values
            .iter()
            .try_fold(0_u64, |total, value| total.checked_add(*value))
    };
    let (Some(input_total), Some(sending_total)) = (sum(inputs), sum(recipients)) else {
        return SendAmounts::default();
    };
    let Some(transaction_total) = sending_total.checked_add(fee) else {
        return SendAmounts::default();
    };
    SendAmounts {
        is_ok: true,
        input_total,
        sending_total,
        transaction_total,
        remaining: sending_total.saturating_sub(input_total),
        change: input_total.saturating_sub(transaction_total),
    }
}

fn decimal_units(text: &str, decimals: u32) -> Option<u64> {
    let (whole, fraction) = text.split_once('.').unwrap_or((text, ""));
    if text.is_empty()
        || (whole.is_empty() && fraction.is_empty())
        || (text.contains('.') && decimals == 0)
        || fraction.len() > decimals as usize
        || !whole
            .bytes()
            .chain(fraction.bytes())
            .all(|c| c.is_ascii_digit())
    {
        return None;
    }
    let whole = if whole.is_empty() {
        0
    } else {
        whole.parse::<u64>().ok()?
    };
    let fraction_value = if fraction.is_empty() {
        0
    } else {
        fraction.parse::<u64>().ok()?
    };
    whole
        .checked_mul(10_u64.pow(decimals))?
        .checked_add(fraction_value.checked_mul(10_u64.pow(decimals - fraction.len() as u32))?)
}

pub fn parse_btc_amount(text: &str) -> AmountParseResult {
    if !text
        .bytes()
        .all(|byte| byte.is_ascii_digit() || byte == b'.')
    {
        return AmountParseResult {
            is_ok: false,
            sats: 0,
        };
    }
    match bitcoin::Amount::from_str_in(text, bitcoin::Denomination::Bitcoin) {
        Ok(amount) if amount > bitcoin::Amount::ZERO && amount <= bitcoin::Amount::MAX_MONEY => {
            AmountParseResult {
                is_ok: true,
                sats: amount.to_sat(),
            }
        }
        _ => AmountParseResult {
            is_ok: false,
            sats: 0,
        },
    }
}

pub fn parse_fee_input(text: &str, rate: bool) -> FeeParseResult {
    let mut result = FeeParseResult::default();
    match (rate, decimal_units(text, if rate { 3 } else { 0 })) {
        (true, Some(msats)) if msats > 0 => {
            let fee_rate = msats as f64 / MSATS_PER_SAT as f64;
            if matches!(fee_rate_msats(fee_rate, 0), Ok(value) if value == msats) {
                result.is_ok = true;
                result.fee_rate = fee_rate;
            }
        }
        (false, Some(sats))
            if sats > 0 && bitcoin::Amount::from_sat(sats) <= bitcoin::Amount::MAX_MONEY =>
        {
            result.is_ok = true;
            result.fee = sats;
        }
        _ => (),
    }
    result
}

pub fn normalize_fee_rate(rate: f64) -> FeeParseResult {
    match fee_rate_msats(rate, 0) {
        Ok(msats) => FeeParseResult {
            is_ok: true,
            fee: 0,
            fee_rate: msats as f64 / MSATS_PER_SAT as f64,
        },
        Err(_) => FeeParseResult::default(),
    }
}
