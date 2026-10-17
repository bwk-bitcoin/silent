//! Regtest chain for the device GUI end to end (`tests/e2e/devices/run.sh`): bitcoind
//! and electrs (blindbitd's setup, as silent's tests use), driven by one command per
//! stdin line, one answer per line. The first line is `READY <electrum url>`.
//!
//!   mine N            -> OK <height>
//!   send ADDR BTC     -> OK <txid>
//!   conf TXID         -> OK <confirmations>
//!   newaddr           -> OK <address>
//!   quit
//!
//! Built as the `e2e_chain` example of the silent crate, for its dev-dependencies.

use std::io::{BufRead, Write};
use std::str::FromStr;

use bwk_utils::test as bwk_test;

fn main() {
    let mut bbd = blindbitd::BlindbitD::new().expect("start blindbitd");
    let electrum = bbd
        .electrsd
        .as_ref()
        .expect("electrsd")
        .electrum_url
        .clone();
    let mut node = bbd.bitcoin().expect("bitcoind");
    bwk_test::generate_blocks(&mut node.client, 101);
    let mut out = std::io::stdout();
    writeln!(out, "READY {electrum}").unwrap();
    out.flush().unwrap();
    for line in std::io::stdin().lock().lines() {
        let line = line.unwrap();
        let words: Vec<&str> = line.split_whitespace().collect();
        let client = &mut node.client;
        let answer = match words.as_slice() {
            ["mine", n] => {
                bwk_test::generate_blocks(client, n.parse().unwrap());
                format!("OK {}", bwk_test::get_height(client))
            }
            ["send", address, btc] => {
                let address = bitcoin::Address::from_str(address)
                    .unwrap()
                    .require_network(bitcoin::Network::Regtest)
                    .unwrap();
                match bwk_test::send(client, address, btc.parse().unwrap()) {
                    Some(txid) => format!("OK {txid}"),
                    None => "ERR send".to_string(),
                }
            }
            ["conf", txid] => {
                let txid = bitcoin::Txid::from_str(txid).unwrap();
                match client.get_raw_transaction_verbose(txid) {
                    Ok(tx) => format!("OK {}", tx.confirmations.unwrap_or(0)),
                    Err(e) => format!("ERR {e}"),
                }
            }
            ["newaddr"] => format!("OK {}", client.new_address().unwrap()),
            ["quit"] => break,
            _ => format!("ERR unknown command '{line}'"),
        };
        writeln!(out, "{answer}").unwrap();
        out.flush().unwrap();
    }
    drop(node);
    drop(bbd);
}
