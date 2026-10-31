//! Structured desktop adapter. Protocol, TLS, mDNS, archive validation and BLE
//! behavior come from Owly; Qt owns consent, KJobs, and the radio lease.
mod awdl;
mod bluez;
use owly_airdrop::{
    driver::{
        mdns::Mdns,
        receiver::{Options, Receiver, ReceiverEvent, Request},
        sender::{Event, Payload, Sender},
        tls::Identity,
    },
    proto::{
        ble::Advert,
        mdns::{self, BrowseEvent, Service},
        messages::Offer,
        receiver::Config,
        sender::{self, Outcome},
    },
};
use serde::Deserialize;
use serde_json::{Value, json};
use std::{
    collections::{HashMap, HashSet},
    io::{self, Write},
    net::SocketAddrV6,
    path::PathBuf,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
};
use tokio::{
    io::{AsyncBufReadExt, BufReader},
    sync::mpsc,
    task::JoinSet,
};
#[derive(Debug, Deserialize)]
#[serde(tag = "command", rename_all = "lowercase", deny_unknown_fields)]
enum Command {
    Accept {
        id: String,
    },
    Decline {
        id: String,
    },
    Cancel {
        id: String,
    },
    Send {
        id: String,
        peer: String,
        paths: Vec<PathBuf>,
    },
    Browse,
    Pause,
    Resume,
    Shutdown,
}
fn emit(value: Value) {
    let stdout = io::stdout();
    let mut out = stdout.lock();
    let _ = serde_json::to_writer(&mut out, &value);
    let _ = out.write_all(b"\n");
    let _ = out.flush();
}
fn random_id() -> io::Result<String> {
    let mut bytes = [0; 6];
    getrandom::fill(&mut bytes).map_err(|e| io::Error::other(e.to_string()))?;
    Ok(mdns::service_id(bytes))
}
struct Arguments {
    interface: String,
    name: String,
    directory: PathBuf,
    burst: Option<mdns::Burst>,
    bus: Option<String>,
}
fn options() -> io::Result<Arguments> {
    let mut args = std::env::args().skip(1);
    let (mut interface, mut name, mut directory) = (None, None, None);
    let (mut count, mut interval, mut pause) = (None, None, None);
    let mut bus = None;
    while let Some(arg) = args.next() {
        let value = args
            .next()
            .ok_or_else(|| io::Error::other("missing argument value"))?;
        match arg.as_str() {
            "--interface" => interface = Some(value),
            "--bus" => bus = Some(value),
            "--name" => name = Some(value),
            "--directory" => directory = Some(PathBuf::from(value)),
            "--announce-count" => count = Some(value.parse::<u32>().map_err(io::Error::other)?),
            "--announce-interval-us" => {
                interval = Some(value.parse::<u64>().map_err(io::Error::other)?)
            }
            "--announce-pause-us" => pause = Some(value.parse::<u64>().map_err(io::Error::other)?),
            _ => return Err(io::Error::other("unknown option")),
        }
    }
    Ok(Arguments {
        interface: interface.ok_or_else(|| io::Error::other("--interface required"))?,
        name: name.unwrap_or_else(|| "Feather".into()),
        directory: directory.ok_or_else(|| io::Error::other("--directory required"))?,
        burst: match (count, interval, pause) {
            (None, None, None) => None,
            (Some(count), Some(interval), Some(pause))
                if count > 0 && interval > 0 && pause > 0 =>
            {
                Some(mdns::Burst {
                    count,
                    interval: std::time::Duration::from_micros(interval),
                    pause: std::time::Duration::from_micros(pause),
                })
            }
            _ => {
                return Err(io::Error::other(
                    "provide all three nonzero announcement timing values",
                ));
            }
        },
        bus,
    })
}
#[derive(Clone)]
struct Peer {
    addr: SocketAddrV6,
    flags: u32,
    name: String,
}
#[tokio::main]
async fn main() {
    if let Err(error) = run().await {
        emit(json!({"event":"fatal","reason":error.to_string()}));
        std::process::exit(1);
    }
}
async fn run() -> io::Result<()> {
    let Arguments {
        interface: iface,
        name,
        directory,
        burst,
        bus,
    } = options()?;
    let _ = rustls::crypto::ring::default_provider().install_default();
    // Interface resolution follows owly-cli/src/airdrop.rs (NET-1/NET-2).
    let index = nix::net::if_::if_nametoindex(iface.as_str()).map_err(io::Error::other)?;
    let addr = nix::ifaddrs::getifaddrs()?
        .filter(|a| a.interface_name == iface)
        .filter_map(|a| a.address?.as_sockaddr_in6().map(|s| s.ip()))
        .find(|ip| ip.segments()[0] & 0xffc0 == 0xfe80)
        .ok_or_else(|| io::Error::other("interface has no link-local IPv6 address"))?;
    let state = std::env::var_os("XDG_STATE_HOME")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|p| PathBuf::from(p).join(".local/state")))
        .ok_or_else(|| io::Error::other("no state directory"))?
        .join("feather");
    let identity = Identity::load_or_generate(&state)?;
    let mut config = Config::new(name.clone());
    config.model_name = "Linux".into();
    // File-only desktop slice: do not advertise URL acceptance.
    let flags = mdns::RECEIVER_FLAGS & !mdns::SUPPORTS_URL;
    let mut receiver_options = Options::new(config, directory);
    receiver_options.report_progress = true;
    let (receiver, mut receiver_events) = Receiver::bind(
        SocketAddrV6::new(addr, 8771, 0, index),
        &identity,
        receiver_options,
    )
    .await?;
    let control = receiver.control();
    let mut receiver_task = tokio::spawn(receiver.run());
    let mdns = Mdns::new(index).with_burst(burst);
    let service_id = random_id()?;
    let service = Service {
        instance: service_id.clone(),
        host: format!("feather-{service_id}"),
        port: 8771,
        addr,
        flags,
    };
    let mut registration = Some(mdns.register(service.clone()).await?);
    let mut browse = mdns.browse().await?;
    let sender = Arc::new(Sender::new(sender::Config::new(
        name.clone(),
        service_id.clone(),
    ))?);
    let (tx, mut updates) = mpsc::unbounded_channel::<Value>();
    let (discovery_stop, stopped) = tokio::sync::watch::channel(false);
    let awdl_task = tokio::spawn(awdl::run(
        iface.clone(),
        index,
        bus,
        sender.clone(),
        tx.clone(),
        stopped,
    ));
    let mut tasks = JoinSet::new();
    let mut requests: HashMap<String, Request> = HashMap::new();
    let mut peers: HashMap<String, Peer> = HashMap::new();
    let mut candidates = HashSet::<String>::new();
    let mut aliases = HashMap::<String, String>::new();
    let mut sends: HashMap<String, Arc<AtomicBool>> = HashMap::new();
    let mut terminal = HashSet::<String>::new();
    let mut cancelling = HashSet::<String>::new();
    let mut deferred_failures = HashMap::<String, Value>::new();
    let mut discovering = HashSet::<String>::new();
    let mut paused = false;
    // BlueZ logic is copied unchanged from Owly, with explicit awaited teardown.
    let mut advert = None;
    match bluez::BluezAdvertiser::new().await {
        Ok(advertiser) => {
            let mut tag = [0; 3];
            getrandom::fill(&mut tag).map_err(|e| io::Error::other(e.to_string()))?;
            match advertiser.advertise(Advert::anonymous(tag)).await {
                Ok(value) => advert = Some(value),
                Err(e) => emit(
                    json!({"event":"warning","reason":format!("Bluetooth discovery unavailable: {e}")}),
                ),
            }
        }
        Err(e) => emit(
            json!({"event":"warning","reason":format!("Bluetooth discovery unavailable: {e}")}),
        ),
    }
    emit(json!({"event":"ready"}));
    let mut lines = BufReader::new(tokio::io::stdin()).lines();
    loop {
        tokio::select! {
         line=lines.next_line()=>{
          let line=match line {Ok(Some(line))=>line,Ok(None)=>break,Err(error)=>{emit(json!({"event":"fatal","reason":error.to_string()}));break;}};
          if line.len()>1024*1024 {emit(json!({"event":"warning","reason":"IPC command exceeds 1 MiB"}));continue;}
          let command=match serde_json::from_str::<Command>(&line){Ok(cmd)=>cmd,Err(e)=>{emit(json!({"event":"warning","reason":format!("Invalid command: {e}")}));continue;}};
          match command {
           Command::Shutdown=>break,
           Command::Pause=>{paused=true;control.pause(true);if let Some(reg)=registration.take(){reg.close().await;}for(_,r)in requests.drain(){let id=format!("in-{}",r.id);r.decline();if terminal.insert(id.clone()){emit(json!({"event":"withdrawn","id":id}));}}},
           Command::Resume=>{if paused{match mdns.register(service.clone()).await{Ok(reg)=>{registration=Some(reg);control.pause(false);paused=false;},Err(error)=>emit(json!({"event":"warning","reason":format!("Cannot resume receiver: {error}")}))}}},
           Command::Browse=>{for(id,peer)in &peers{emit(json!({"event":"peer","id":id,"name":peer.name,"address":peer.addr.to_string(),"flags":peer.flags}));}if let Ok(next)=mdns.browse().await{let old=std::mem::replace(&mut browse,next);old.close().await;}},
           Command::Accept{id}=>{if !paused && let Some(request)=requests.remove(&id){let sender=request.ask.sender_computer_name.clone();request.accept();emit(json!({"event":"started","id":id,"direction":"incoming","name":sender}));}},
           Command::Decline{id}=>{if let Some(request)=requests.remove(&id){request.decline();if terminal.insert(id.clone()){emit(json!({"event":"cancelled","id":id}));}}},
           Command::Cancel{id}=>{
            if let Some(cancel)=sends.get(&id){cancel.store(true,Ordering::Release);}
            else if let Some(request)=requests.remove(&id){request.decline();if terminal.insert(id.clone()){emit(json!({"event":"cancelled","id":id}));}}
            else if let Some(raw)=id.strip_prefix("in-").and_then(|id|id.parse().ok()){
             if !cancelling.insert(id.clone()){continue;}let control=control.clone();let tx=tx.clone();tasks.spawn(async move{let cancelled=control.cancel(raw).await;let _=tx.send(json!({"event":"cancel-result","id":id,"cancelled":cancelled}));});
            }
           },
           Command::Send{id,peer,paths}=>{
            if !id.starts_with("out-") || sends.contains_key(&id) || terminal.contains(&id){continue;}
            let Some(target)=peers.get(&peer).cloned()else{terminal.insert(id.clone());emit(json!({"event":"failed","id":id,"reason":"Recipient expired; browse again"}));continue;};
            if paths.is_empty() || paths.iter().any(|p|!p.is_absolute()){terminal.insert(id.clone());emit(json!({"event":"failed","id":id,"reason":"Send requires absolute local file paths"}));continue;}
            emit(json!({"event":"started","id":id,"direction":"outgoing","name":peer}));
            let cancel=Arc::new(AtomicBool::new(false));sends.insert(id.clone(),cancel.clone());let sender=sender.clone();let tx=tx.clone();
            tasks.spawn(async move{
             let(events,mut rx)=mpsc::unbounded_channel();let progress_tx=tx.clone();let progress_id=id.clone();
             let progress=tokio::spawn(async move{while let Some(event)=rx.recv().await{if let Event::Progress(p)=event{let _=progress_tx.send(json!({"event":"progress","id":progress_id,"done":p.sent,"total":p.total}));}}});
             let result=sender.send_cancellable(target.addr,&Payload::Files(paths),target.flags,Some(events),cancel.clone()).await;
             let _=progress.await;
             let value=match result{Ok(Outcome::Delivered)=>json!({"event":"completed","id":id}),Ok(Outcome::Declined)=>json!({"event":"failed","id":id,"reason":"Recipient declined or consent timed out"}),Ok(Outcome::Failed(reason))=>json!({"event":"failed","id":id,"reason":reason}),Err(e)if e.kind()==io::ErrorKind::Interrupted=>json!({"event":"cancelled","id":id}),Err(e)=>json!({"event":"failed","id":id,"reason":e.to_string()})};let _=tx.send(value);
            });
           }
          }
         },
         event=receiver_events.recv()=>{let Some(event)=event else{break;};match event{
          ReceiverEvent::Request(request)=>{let id=format!("in-{}",request.id);if paused{request.decline();continue;}let files=match &request.ask.offer{Offer::Files(files)=>files.iter().map(|f|f.name.clone()).collect::<Vec<_>>(),Offer::Urls(_)=>{request.decline();continue;}};emit(json!({"event":"request","id":id,"sender":request.ask.sender_computer_name,"files":files}));requests.insert(id,request);},
          ReceiverEvent::Withdrawn{id}=>{let id=format!("in-{id}");requests.remove(&id);if terminal.insert(id.clone()){emit(json!({"event":"withdrawn","id":id}));}},
          ReceiverEvent::Progress{id,done}=>{let id=format!("in-{id}");if !terminal.contains(&id){emit(json!({"event":"progress","id":id,"done":done}));}},
          ReceiverEvent::Received{id,paths}=>{let id=format!("in-{id}");if terminal.insert(id.clone()){emit(json!({"event":"completed","id":id,"paths":paths}));}},
          ReceiverEvent::Failed{id,reason}=>{let id=format!("in-{id}");let value=json!({"event":"failed","id":id,"reason":reason});if cancelling.contains(&id){deferred_failures.insert(id,value);}else if terminal.insert(id){emit(value);}},
          ReceiverEvent::Urls{..}|ReceiverEvent::Discovered{..}=>{},
         }},
         event=browse.events.recv()=>{if let Some(event)=event{match event{
          BrowseEvent::Found(found)=>{candidates.insert(found.instance.clone());if found.instance==service_id || discovering.contains(&found.instance){continue;}discovering.insert(found.instance.clone());let sender=sender.clone();let tx=tx.clone();let addr=SocketAddrV6::new(found.addr,found.port,0,index);tasks.spawn(async move{match sender.discover(addr).await{Ok(Some(receiver))=>{let _=tx.send(json!({"event":"peer","id":found.instance,"name":receiver.computer_name,"address":addr.to_string(),"flags":found.flags}));},_=>{let _=tx.send(json!({"event":"discovery-finished","id":found.instance}));}}});},
          BrowseEvent::Lost(id)=>{candidates.remove(&id);if let Some(id)=remove_alias(&id,&mut aliases,&mut peers){emit(json!({"event":"peer-lost","id":id}));}},
         }}},
         Some(value)=updates.recv()=>{
          let id=value["id"].as_str().unwrap_or("").to_string();let event=value["event"].as_str().unwrap_or("");
          match event{
           "cancel-result"=>{if let Some(value)=resolve_cancel(&id,value["cancelled"].as_bool().unwrap_or(false),&mut terminal,&mut cancelling,&mut deferred_failures){emit(value);}},
           "peer"|"awdl-peer"=>{discovering.remove(&id);if event=="peer"&&!candidates.contains(&id){continue;}
             if let Some(value)=register_peer(value,&mut aliases,&mut peers){emit(value);}},
           "awdl-snapshot"=>{let active=value["ids"].as_array().into_iter().flatten().filter_map(|id|id.as_str()).collect::<HashSet<_>>();let stale=aliases.keys().filter(|id|id.starts_with("awdl-")&&!active.contains(id.as_str())).cloned().collect::<Vec<_>>();for id in stale{if let Some(id)=remove_alias(&id,&mut aliases,&mut peers){emit(json!({"event":"peer-lost","id":id}));}}},
           "discovery-finished"=>{discovering.remove(&id);},
           "completed"|"failed"|"cancelled"=>{sends.remove(&id);if terminal.insert(id){emit(value);}},
           _=>{if !terminal.contains(&id){emit(value);}},
          }
         },
         Some(_)=tasks.join_next(),if !tasks.is_empty()=>{},
         _=tokio::signal::ctrl_c()=>break,
        }
    }
    let _ = discovery_stop.send(true);
    let _ = awdl_task.await;
    if let Some(reg) = registration.take() {
        reg.close().await;
    }
    browse.close().await;
    for (id, request) in requests.drain() {
        request.decline();
        if terminal.insert(id.clone()) {
            emit(json!({"event":"withdrawn","id":id}));
        }
    }
    control.pause(true);
    control.shutdown();
    for cancel in sends.values() {
        cancel.store(true, Ordering::Release);
    }
    loop {
        tokio::select! {
            _ = &mut receiver_task => break,
            Some(event) = receiver_events.recv() => shutdown_receiver(event, &mut terminal, &cancelling, &mut deferred_failures),
        }
    }
    while let Ok(event) = receiver_events.try_recv() {
        shutdown_receiver(event, &mut terminal, &cancelling, &mut deferred_failures);
    }
    while tasks.join_next().await.is_some() {}
    // Flush terminal updates before exit; no detached sender/preparation task remains.
    while let Ok(value) = updates.try_recv() {
        let id = value["id"].as_str().unwrap_or("").to_string();
        if value["event"] == "cancel-result" {
            if let Some(value) = resolve_cancel(
                &id,
                value["cancelled"].as_bool().unwrap_or(false),
                &mut terminal,
                &mut cancelling,
                &mut deferred_failures,
            ) {
                emit(value);
            }
            continue;
        }
        if matches!(
            value["event"].as_str(),
            Some("completed" | "failed" | "cancelled")
        ) && terminal.insert(id)
        {
            emit(value);
        }
    }
    if let Some(advert) = advert {
        advert.close().await;
    }
    emit(json!({"event":"stopped"}));
    Ok(())
}
// Preserve the first listed ID when mDNS and AWDL discover the same IPv6 peer.
fn register_peer(
    mut value: Value,
    aliases: &mut HashMap<String, String>,
    peers: &mut HashMap<String, Peer>,
) -> Option<Value> {
    let source = value["id"].as_str()?.to_string();
    let addr = value["address"].as_str()?.parse::<SocketAddrV6>().ok()?;
    let stable = peers
        .iter()
        .find(|(_, peer)| peer.addr.ip() == addr.ip() && peer.addr.scope_id() == addr.scope_id())
        .map(|(id, _)| id.clone())
        .unwrap_or_else(|| source.clone());
    aliases.insert(source, stable.clone());
    peers.insert(
        stable.clone(),
        Peer {
            addr,
            flags: value["flags"].as_u64().unwrap_or(0) as u32,
            name: value["name"].as_str().unwrap_or("").to_string(),
        },
    );
    value["id"] = json!(stable);
    value["event"] = json!("peer");
    Some(value)
}
fn remove_alias(
    source: &str,
    aliases: &mut HashMap<String, String>,
    peers: &mut HashMap<String, Peer>,
) -> Option<String> {
    let stable = aliases.remove(source)?;
    if aliases.values().any(|id| id == &stable) {
        return None;
    }
    peers.remove(&stable);
    Some(stable)
}
fn shutdown_receiver(
    event: ReceiverEvent,
    terminal: &mut HashSet<String>,
    cancelling: &HashSet<String>,
    deferred_failures: &mut HashMap<String, Value>,
) {
    let value = match event {
        ReceiverEvent::Received { id, paths } => {
            json!({"event":"completed","id":format!("in-{id}"),"paths":paths})
        }
        ReceiverEvent::Failed { id, reason } => {
            let id = format!("in-{id}");
            let value = json!({"event":"failed","id":id,"reason":reason});
            if cancelling.contains(&id) {
                deferred_failures.insert(id, value);
                return;
            }
            value
        }
        ReceiverEvent::Withdrawn { id } => json!({"event":"withdrawn","id":format!("in-{id}")}),
        _ => return,
    };
    let id = value["id"].as_str().unwrap().to_string();
    if terminal.insert(id) {
        emit(value);
    }
}
/// A cancellation that lost to a final commit must preserve its natural result,
/// including a failure delivered before the control future resolves.
fn resolve_cancel(
    id: &str,
    cancelled: bool,
    terminal: &mut HashSet<String>,
    cancelling: &mut HashSet<String>,
    deferred: &mut HashMap<String, Value>,
) -> Option<Value> {
    cancelling.remove(id);
    let failure = deferred.remove(id);
    let result = if cancelled {
        Some(json!({"event":"cancelled","id":id}))
    } else {
        failure
    };
    result.filter(|_| terminal.insert(id.to_string()))
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn mdns_and_awdl_share_stable_id_until_both_sources_expire() {
        let mut aliases = HashMap::new();
        let mut peers = HashMap::new();
        let first=register_peer(json!({"event":"awdl-peer","id":"awdl-mac","name":"Phone","address":"[fe80::1234%7]:8770","flags":139}),&mut aliases,&mut peers).unwrap();
        assert_eq!(first["id"], "awdl-mac");
        let second=register_peer(json!({"event":"peer","id":"mdns-id","name":"Phone","address":"[fe80::1234%7]:8770","flags":137}),&mut aliases,&mut peers).unwrap();
        assert_eq!(second["id"], "awdl-mac");
        assert_eq!(peers.len(), 1);
        assert!(remove_alias("awdl-mac", &mut aliases, &mut peers).is_none());
        assert_eq!(peers.len(), 1);
        assert_eq!(
            remove_alias("mdns-id", &mut aliases, &mut peers).as_deref(),
            Some("awdl-mac")
        );
        assert!(peers.is_empty());
    }
    #[test]
    fn same_ipv6_on_distinct_scopes_is_not_deduplicated() {
        let mut aliases = HashMap::new();
        let mut peers = HashMap::new();
        for (id, scope) in [("one", 7), ("two", 8)] {
            register_peer(json!({"id":id,"name":"Phone","address":format!("[fe80::1234%{scope}]:8770"),"flags":137}),&mut aliases,&mut peers).unwrap();
        }
        assert_eq!(peers.len(), 2);
    }

    #[test]
    fn refused_cancel_preserves_failure_already_delivered() {
        let mut terminal = HashSet::new();
        let mut cancelling = HashSet::from(["in-1".to_string()]);
        let mut deferred = HashMap::from([(
            "in-1".to_string(),
            json!({"event":"failed","id":"in-1","reason":"commit failed"}),
        )]);
        let event =
            resolve_cancel("in-1", false, &mut terminal, &mut cancelling, &mut deferred).unwrap();
        assert_eq!(event["event"], "failed");
        assert_eq!(event["reason"], "commit failed");
        assert!(!cancelling.contains("in-1"));
        assert!(terminal.contains("in-1"));
        assert!(
            resolve_cancel("in-1", true, &mut terminal, &mut cancelling, &mut deferred).is_none()
        );
    }
    #[test]
    fn successful_cancel_replaces_connection_failure_after_cleanup() {
        let mut terminal = HashSet::new();
        let mut cancelling = HashSet::from(["in-1".to_string()]);
        let mut deferred =
            HashMap::from([("in-1".to_string(), json!({"event":"failed","id":"in-1"}))]);
        let event =
            resolve_cancel("in-1", true, &mut terminal, &mut cancelling, &mut deferred).unwrap();
        assert_eq!(event["event"], "cancelled");
        assert!(deferred.is_empty());
    }
    #[test]
    fn commands_are_structured_and_strict() {
        assert!(serde_json::from_str::<Command>(r#"{"command":"accept","id":"in-1"}"#).is_ok());
        assert!(
            serde_json::from_str::<Command>(r#"{"command":"accept","id":"in-1","shell":"rm"}"#)
                .is_err()
        );
        assert!(
            serde_json::from_str::<Command>(
                r#"{"command":"send","id":"out-1","peer":"one","paths":["/tmp/x"]}"#
            )
            .is_ok()
        );
    }
}
