//! Regtest chain for the device GUI end to end (`tests/e2e/devices/run.sh`): bitcoind
//! and electrs (blindbitd's setup, as silent's tests use), driven by one command per
//! stdin line, one answer per line. The first line is `READY <electrum url>`.
//!
//!   mine N            -> OK <height>
//!   send ADDR BTC     -> OK <txid>
//!   conf TXID         -> OK <confirmations>
//!   newaddr           -> OK <address>
//!   relay             -> OK <port>: starts (once) a TCP relay to electrs, for a
//!                        server that goes away while the app is connected
//!   relay drop        -> OK <n>: closes the n connections it relays, as a server
//!                        restarting does; new ones are relayed again
//!   relay mute        -> OK muted: keeps every connection open but relays nothing either
//!                        way, as a silently dead link (sleep, NAT, wifi lost) does
//!   relay unmute      -> OK unmuted: relays again
//!   quit
//!
//! Built as the `e2e_chain` example of the silent crate, for its dev-dependencies.

use std::io::{BufRead, Read, Write};
use std::net::{Shutdown, TcpListener, TcpStream};
use std::str::FromStr;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;

use bwk_utils::test as bwk_test;

/// A TCP relay to `upstream`, whose connections can be dropped or muted.
struct Relay {
    port: u16,
    muted: Arc<AtomicBool>,
    clients: Arc<Mutex<Vec<TcpStream>>>,
}

impl Relay {
    fn start(upstream: String) -> Relay {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind relay");
        let port = listener.local_addr().expect("relay address").port();
        let muted = Arc::new(AtomicBool::new(false));
        let clients: Arc<Mutex<Vec<TcpStream>>> = Arc::new(Mutex::new(Vec::new()));
        let (muted_in, clients_in) = (muted.clone(), clients.clone());
        thread::spawn(move || {
            for client in listener.incoming().flatten() {
                let Ok(server) = TcpStream::connect(&upstream) else {
                    continue;
                };
                clients_in
                    .lock()
                    .unwrap()
                    .push(client.try_clone().expect("clone client"));
                pump(&client, &server, muted_in.clone());
                pump(&server, &client, muted_in.clone());
            }
        });
        Relay {
            port,
            muted,
            clients,
        }
    }

    fn drop_all(&self) -> usize {
        let mut clients = self.clients.lock().unwrap();
        let count = clients.len();
        for client in clients.drain(..) {
            let _ = client.shutdown(Shutdown::Both);
        }
        count
    }
}

/// Copies `from` to `to` until either side closes; muted, what it reads is dropped.
fn pump(from: &TcpStream, to: &TcpStream, muted: Arc<AtomicBool>) {
    let (mut from, mut to) = (
        from.try_clone().expect("clone"),
        to.try_clone().expect("clone"),
    );
    thread::spawn(move || {
        let mut buffer = [0u8; 16 * 1024];
        loop {
            match from.read(&mut buffer) {
                Ok(0) | Err(_) => break,
                Ok(n) => {
                    if !muted.load(Ordering::SeqCst) && to.write_all(&buffer[..n]).is_err() {
                        break;
                    }
                }
            }
        }
        let _ = to.shutdown(Shutdown::Both);
    });
}

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
    let mut relay: Option<Relay> = None;
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
            ["relay"] => {
                let upstream = electrum.trim_start_matches("tcp://").to_string();
                format!(
                    "OK {}",
                    relay.get_or_insert_with(|| Relay::start(upstream)).port
                )
            }
            ["relay", "drop"] => match &relay {
                Some(relay) => format!("OK {}", relay.drop_all()),
                None => "ERR no relay".to_string(),
            },
            ["relay", mode @ ("mute" | "unmute")] => match &relay {
                Some(relay) => {
                    relay.muted.store(*mode == "mute", Ordering::SeqCst);
                    format!("OK {mode}d")
                }
                None => "ERR no relay".to_string(),
            },
            ["quit"] => break,
            _ => format!("ERR unknown command '{line}'"),
        };
        writeln!(out, "{answer}").unwrap();
        out.flush().unwrap();
    }
    drop(node);
    drop(bbd);
}
