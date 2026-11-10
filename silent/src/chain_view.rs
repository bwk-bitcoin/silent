//! What a Multikey account's blockchain backend plugin reports: the outputs paying the
//! account's descriptor, how far each branch is used and the chain height. Fed from the
//! backend's IBlockchain on coinsChanged and stateChanged, it stands in for the Electrum
//! scanner's coins, used indexes and tip while the account syncs from that plugin.

use std::{collections::BTreeMap, str::FromStr};

use bitcoin::{Amount, OutPoint, ScriptBuf, Sequence, TxOut};
use bwk::{
    bwk_coin::{
        max_input_satisfaction_size, Coin, CoinSource, CoinSpendInfo, CoinStatus, KeyChain,
    },
    bwk_descriptor::{
        spend::Maturity,
        wallet::{WalletDescriptor, WalletDescriptorExt},
    },
};

use crate::{
    ffi::{self, ChainOutput, CoinState},
    spending::Funding,
};

/// A Multikey account watches its one descriptor, the first of its AccountWatch.
pub(crate) const ACCOUNT_DESCRIPTOR: u32 = 0;
/// The multipath branches of that descriptor.
const BRANCHES: usize = 2;

/// One output the backend reports, as a coin bwk builds transactions from.
#[derive(Debug, Clone)]
struct ChainCoin {
    coin: Coin,
    coinbase: bool,
}

#[derive(Debug, Clone)]
pub(crate) struct ChainView {
    coins: BTreeMap<OutPoint, ChainCoin>,
    /// The highest used index of the receive, then the change branch.
    used: [Option<u32>; BRANCHES],
    gap_limit: u32,
    tip: Option<u32>,
}

impl ChainView {
    /// The backend's report for the account of `descriptor`. Refused whole when it names
    /// a descriptor or branch the account does not have, or an output whose script is not
    /// the one its index derives.
    pub(crate) fn parse(
        view: ffi::ChainView,
        descriptor: &WalletDescriptor,
    ) -> Result<Self, String> {
        let satisfaction = max_input_satisfaction_size(descriptor) as u64;
        let mut coins = BTreeMap::new();
        for output in view.outputs {
            let coin = chain_coin(output, descriptor, satisfaction)?;
            let outpoint = coin.coin.outpoint;
            if coins.insert(outpoint, coin).is_some() {
                return Err(format!("The blockchain backend reports {outpoint} twice"));
            }
        }
        let mut used = [None; BRANCHES];
        let mut reported = [false; BRANCHES];
        for entry in view.used {
            let branch = branch(entry.descriptor, entry.keychain)?;
            if std::mem::replace(&mut reported[branch], true) {
                return Err(format!(
                    "The blockchain backend reports branch {} twice",
                    entry.keychain
                ));
            }
            used[branch] = entry.used.then_some(entry.last);
        }
        let tip = if view.has_tip {
            Some(
                u32::try_from(view.tip)
                    .map_err(|_| format!("Invalid chain height {}", view.tip))?,
            )
        } else {
            None
        };
        Ok(Self {
            coins,
            used,
            gap_limit: view.gap_limit,
            tip,
        })
    }

    /// The coin at `outpoint`, spent or not.
    pub(crate) fn coin(&self, outpoint: &OutPoint) -> Option<&Coin> {
        self.coins.get(outpoint).map(|coin| &coin.coin)
    }

    /// Whether the coin at `outpoint` can be spent in the next block: unspent, and a
    /// coinbase only once mature at the backend's tip.
    pub(crate) fn spendable(&self, outpoint: &OutPoint) -> bool {
        self.coins
            .get(outpoint)
            .is_some_and(|coin| self.spendable_coin(coin))
    }

    fn spendable_coin(&self, coin: &ChainCoin) -> bool {
        !matches!(coin.coin.status, CoinStatus::Spent | CoinStatus::BeingSpend)
            && self.funding(coin).coinbase_maturity() == Maturity::Mature
    }

    /// Where the funding of `coin` sits below the backend's tip.
    fn funding(&self, coin: &ChainCoin) -> Funding {
        Funding::at_height(self.tip, coin.coin.height.map(|h| h as u32), coin.coinbase)
    }

    /// The coins coin selection may pick.
    pub(crate) fn coin_source(&self) -> Box<dyn CoinSource> {
        Box::new(ChainCoins(
            self.coins
                .values()
                .filter(|coin| self.spendable_coin(coin))
                .map(|coin| coin.coin.clone())
                .collect(),
        ))
    }

    /// Every coin with where its funding sits, for the spending export.
    pub(crate) fn fundings(&self) -> impl Iterator<Item = (&Coin, Funding)> {
        self.coins
            .values()
            .map(|coin| (&coin.coin, self.funding(coin)))
    }

    /// The spendable coins and their totals, confirmed apart.
    pub(crate) fn coin_state(&self) -> CoinState {
        let mut state = CoinState {
            confirmed_count: 0,
            confirmed_balance: 0,
            unconfirmed_count: 0,
            unconfirmed_balance: 0,
        };
        for coin in self.coins.values().filter(|coin| self.spendable_coin(coin)) {
            let value = coin.coin.value().to_sat();
            if coin.coin.height.is_some() {
                state.confirmed_count += 1;
                state.confirmed_balance += value;
            } else {
                state.unconfirmed_count += 1;
                state.unconfirmed_balance += value;
            }
        }
        state
    }

    pub(crate) fn tip(&self) -> Option<u32> {
        self.tip
    }

    /// The last index of `keychain` either the chain used or the account handed out,
    /// `handed_out`: the next address of that branch is the one after.
    pub(crate) fn branch_tip(&self, keychain: KeyChain, handed_out: u32) -> u32 {
        self.last_used(keychain)
            .map_or(handed_out, |used| used.max(handed_out))
    }

    /// Whether the backend watches `index` of `keychain` already: the gap limit's unused
    /// indexes past the last used one, the first ones while none is used.
    pub(crate) fn watches(&self, keychain: KeyChain, index: u32) -> bool {
        match self.last_used(keychain) {
            Some(last) => u64::from(index) <= u64::from(last) + u64::from(self.gap_limit),
            None => index < self.gap_limit,
        }
    }

    /// The highest used index of `keychain`, none on a custom branch, which the account
    /// does not have.
    fn last_used(&self, keychain: KeyChain) -> Option<u32> {
        match keychain {
            KeyChain::Receive => self.used[0],
            KeyChain::Change => self.used[1],
            KeyChain::Custom(_) => None,
        }
    }
}

/// The coins of a chain view, as a TxBuilder picks from them.
struct ChainCoins(Vec<Coin>);

impl CoinSource for ChainCoins {
    fn spendable_coins(&self) -> Vec<Coin> {
        self.0.clone()
    }
}

/// The multipath branch `keychain` of the account's descriptor, as an index.
fn branch(descriptor: u32, keychain: u32) -> Result<usize, String> {
    if descriptor != ACCOUNT_DESCRIPTOR {
        return Err(format!(
            "The blockchain backend reports descriptor {descriptor}: the account watches one"
        ));
    }
    let branch = keychain as usize;
    if branch >= BRANCHES {
        return Err(format!(
            "The blockchain backend reports branch {keychain}: the account has a receive and a \
             change branch"
        ));
    }
    Ok(branch)
}

/// `output` as a coin of `descriptor`, its witness UTXO the reported amount and script and
/// its derivation the reported branch and index.
fn chain_coin(
    output: ChainOutput,
    descriptor: &WalletDescriptor,
    satisfaction: u64,
) -> Result<ChainCoin, String> {
    let branch = branch(output.descriptor, output.keychain)?;
    let outpoint = OutPoint::from_str(&output.outpoint)
        .map_err(|e| format!("Invalid outpoint '{}': {e}", output.outpoint))?;
    let script = ScriptBuf::from_bytes(output.script_pubkey);
    let derived = descriptor
        .single_paths()
        .map_err(|e| e.to_string())?
        .get(branch)
        .ok_or("The account descriptor has no such branch")?
        .derive_at(output.index)
        .map_err(|e| e.to_string())?
        .script_pubkey();
    if script != derived {
        return Err(format!(
            "The blockchain backend reports {outpoint} paying a script branch {} index {} does \
             not derive",
            output.keychain, output.index
        ));
    }
    let keychain = KeyChain::from(output.keychain);
    let status = match (output.spent_by.is_empty(), output.height) {
        (false, _) => CoinStatus::Spent,
        (true, 0) => CoinStatus::Unconfirmed,
        (true, _) => CoinStatus::Confirmed,
    };
    Ok(ChainCoin {
        coin: Coin {
            txout: TxOut {
                value: Amount::from_sat(output.amount),
                script_pubkey: script,
            },
            outpoint,
            height: (output.height != 0).then_some(output.height),
            // Overwritten at spend time, as the scanner's coins.
            sequence: Sequence::ZERO,
            status,
            label: None,
            satisfaction_size: satisfaction,
            spend_info: CoinSpendInfo::Bip32 {
                coin_path: (keychain, output.index),
                descriptor: descriptor.clone(),
            },
        },
        coinbase: output.coinbase,
    })
}
