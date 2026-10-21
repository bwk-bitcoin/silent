use bwk::bwk_tx::transaction::{Amount, TransactionResult};

use crate::ffi::TransactionSimulation;

pub(crate) fn simulation_preview(
    result: TransactionResult,
    fee_rate: f64,
) -> TransactionSimulation {
    match preview(result, fee_rate) {
        Ok(preview) => preview,
        Err(error) => TransactionSimulation {
            error,
            kind: crate::ffi::BackendErrorKind::InvalidInput,
            ..Default::default()
        },
    }
}

fn preview(result: TransactionResult, fee_rate: f64) -> Result<TransactionSimulation, String> {
    if let Some(error) = result.error {
        return Err(format!("{error:?}"));
    }
    let selected_values: Vec<_> = result
        .tx_template
        .inputs
        .iter()
        .map(|coin| coin.txout.value.to_sat())
        .collect();
    let selected_outpoints = result
        .tx_template
        .inputs
        .iter()
        .map(|coin| coin.outpoint.to_string())
        .collect();
    let input_total = sum(&selected_values)?;
    let recipient_amounts = result
        .tx_template
        .outputs
        .iter()
        .map(|recipient| match recipient.amount() {
            Amount::Value(value) | Amount::Max(Some(value)) => Ok(value),
            Amount::Anchor => Ok(0),
            Amount::Max(None) => Err("builder returned an unresolved MAX output".into()),
        })
        .collect::<Result<Vec<_>, String>>()?;
    let sending_total = sum(&recipient_amounts)?;
    let change = result.change.map(|amount| amount.to_sat()).unwrap_or(0);
    let fee = result.fees.ok_or("builder returned no fee")?.to_sat();
    let transaction_total = sending_total
        .checked_add(fee)
        .ok_or("transaction total overflow")?;
    let output_total = sending_total
        .checked_add(change)
        .ok_or("output total overflow")?;
    if transaction_total.checked_add(change) != Some(input_total) {
        return Err("builder preview totals do not balance".into());
    }
    let weight = if fee_rate > 0.0 {
        (fee as f64 / fee_rate).ceil() as u64 * 4
    } else {
        0
    };
    Ok(TransactionSimulation {
        is_valid: true,
        kind: crate::ffi::BackendErrorKind::None,
        fee,
        weight,
        input_total,
        output_total,
        input_count: selected_values.len() as u64,
        error: String::new(),
        selected_outpoints,
        selected_values,
        recipient_amounts,
        sending_total,
        change,
        transaction_total,
    })
}

fn sum(values: &[u64]) -> Result<u64, String> {
    values.iter().try_fold(0_u64, |total, value| {
        total
            .checked_add(*value)
            .ok_or("amount total overflow".into())
    })
}
