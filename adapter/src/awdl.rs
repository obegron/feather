//! Radio1 read-only peer fallback, adapted from Owly CLI `knock_peers`.
//! Modified EUI-64 conversion follows owly-awdl::MacAddr::link_local (MIT).
use owly_airdrop::{
    driver::sender::Sender,
    proto::mdns::{RECEIVER_FLAGS, SUPPORTS_DVZIP},
};
use serde_json::{Value, json};
use std::{
    collections::{HashMap, HashSet},
    net::{Ipv6Addr, SocketAddrV6},
    sync::Arc,
    time::Duration,
};
use tokio::{
    sync::{Semaphore, mpsc, watch},
    task::JoinSet,
    time::{Instant, timeout},
};
use zbus::zvariant::OwnedValue;
#[zbus::proxy(
    interface = "io.github.t4t5.Owly.Radio1",
    default_service = "io.github.t4t5.Owly",
    default_path = "/io/github/t4t5/Owly"
)]
trait Radio {
    #[zbus(property)]
    fn link(&self) -> zbus::Result<HashMap<String, OwnedValue>>;
    #[zbus(property)]
    fn peers(&self) -> zbus::Result<Vec<HashMap<String, OwnedValue>>>;
}
pub fn address(mac: &str, scope: u32) -> Option<(String, SocketAddrV6)> {
    let parts = mac.split(':').collect::<Vec<_>>();
    if parts.len() != 6 {
        return None;
    }
    let mut bytes = [0u8; 6];
    for (slot, part) in bytes.iter_mut().zip(parts) {
        if part.len() != 2 {
            return None;
        }
        *slot = u8::from_str_radix(part, 16).ok()?;
    }
    if bytes[0] & 1 != 0 || bytes == [0; 6] {
        return None;
    }
    let ip = Ipv6Addr::new(
        0xfe80,
        0,
        0,
        0,
        u16::from_be_bytes([bytes[0] ^ 2, bytes[1]]),
        u16::from_be_bytes([bytes[2], 0xff]),
        u16::from_be_bytes([0xfe, bytes[3]]),
        u16::from_be_bytes([bytes[4], bytes[5]]),
    );
    let id = format!(
        "awdl-{}",
        bytes.iter().map(|b| format!("{b:02x}")).collect::<String>()
    );
    Some((id, SocketAddrV6::new(ip, 8770, 0, scope)))
}
pub async fn run(
    interface: String,
    scope: u32,
    bus: Option<String>,
    sender: Arc<Sender>,
    tx: mpsc::UnboundedSender<Value>,
    mut stop: watch::Receiver<bool>,
) {
    let mut connection = None;
    let mut confirmed = HashMap::<String, Instant>::new();
    let mut rotation = 0usize;
    loop {
        if *stop.borrow() {
            break;
        }
        let round = async {
            if connection.is_none() {
                connection = timeout(Duration::from_secs(2), async {
                    match &bus {
                        Some(address) => {
                            zbus::connection::Builder::address(address.as_str())?
                                .build()
                                .await
                        }
                        None => zbus::Connection::system().await,
                    }
                })
                .await
                .ok()
                .and_then(Result::ok);
            }
            let Some(conn) = &connection else {
                return Vec::new();
            };
            let result = timeout(Duration::from_secs(2), async {
                let radio = RadioProxy::builder(conn)
                    .cache_properties(zbus::proxy::CacheProperties::No)
                    .build()
                    .await?;
                let link = radio.link().await?;
                if link
                    .get("interface")
                    .and_then(|v| v.downcast_ref::<String>().ok())
                    .as_deref()
                    != Some(interface.as_str())
                {
                    return Ok::<_, zbus::Error>(Vec::new());
                }
                Ok(radio
                    .peers()
                    .await?
                    .into_iter()
                    .take(256)
                    .filter_map(|peer| {
                        address(&peer.get("mac")?.downcast_ref::<String>().ok()?, scope)
                    })
                    .collect::<Vec<_>>())
            })
            .await;
            match result {
                Ok(Ok(peers)) => peers,
                _ => {
                    connection = None;
                    Vec::new()
                }
            }
        };
        let heard = tokio::select! {value=round=>value,_=stop.changed()=>break};
        let heard_ids = heard
            .iter()
            .map(|(id, _)| id.clone())
            .collect::<HashSet<_>>();
        confirmed
            .retain(|id, last| heard_ids.contains(id) && last.elapsed() < Duration::from_secs(15));
        let mut probes = JoinSet::new();
        let semaphore = Arc::new(Semaphore::new(4));
        // At most eight probes per round, four concurrently; rotate large peer sets.
        for (id, addr) in heard
            .iter()
            .cycle()
            .skip(rotation)
            .take(heard.len().min(8))
            .cloned()
        {
            let sender = sender.clone();
            let semaphore = semaphore.clone();
            probes.spawn(async move {
                let _permit = semaphore.acquire_owned().await.ok()?;
                let receiver = timeout(Duration::from_secs(3), sender.discover(addr))
                    .await
                    .ok()?
                    .ok()??;
                Some((id, addr, receiver.computer_name))
            });
        }
        if !heard.is_empty() {
            rotation = (rotation + 8) % heard.len();
        }
        while !probes.is_empty() {
            tokio::select! {
             result=probes.join_next()=>{if let Some(Ok(Some((id,addr,name))))=result{confirmed.insert(id.clone(),Instant::now());let _=tx.send(json!({"event":"awdl-peer","id":id,"name":name,"address":addr.to_string(),"flags":RECEIVER_FLAGS|SUPPORTS_DVZIP}));}},
             _=stop.changed()=>{probes.abort_all();while probes.join_next().await.is_some(){}return;},
            }
        }
        let ids = confirmed.keys().cloned().collect::<Vec<_>>();
        let _ = tx.send(json!({"event":"awdl-snapshot","ids":ids}));
        tokio::select! {_=tokio::time::sleep(Duration::from_millis(500))=>{},_=stop.changed()=>break}
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn modified_eui64_and_scope_match_owly() {
        let (id, addr) = address("86:08:04:8A:2B:F4", 7).unwrap();
        assert_eq!(id, "awdl-8608048a2bf4");
        assert_eq!(
            *addr.ip(),
            "fe80::8408:4ff:fe8a:2bf4".parse::<Ipv6Addr>().unwrap()
        );
        assert_eq!(addr.scope_id(), 7);
        assert_eq!(addr.port(), 8770);
    }
    #[test]
    fn invalid_and_multicast_macs_are_not_probed() {
        for text in [
            "bad",
            "00:00:00:00:00:00",
            "ff:ff:ff:ff:ff:ff",
            "01:00:5e:00:00:01",
            "86:08:04:8a:2b:fff",
        ] {
            assert!(address(text, 1).is_none(), "{text}");
        }
    }
}
