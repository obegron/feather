//! The receiver driver: accepts TLS connections and runs a
//! [`proto::receiver::Connection`](crate::proto::receiver::Connection) on
//! each, asking the frontend about every `/Ask` and writing uploads through
//! [`Staging`].

use std::collections::HashMap;
use std::io;
use std::net::{Ipv6Addr, SocketAddrV6};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::{mpsc, oneshot};
use tokio::task::{AbortHandle, JoinSet};
use tokio::time::{Instant, sleep_until, timeout};
use tokio_rustls::TlsAcceptor;

use super::staging::Staging;
use super::tls::{self, Identity};
use crate::proto::messages::{AskRequest, DiscoverRequest, Offer};
use crate::proto::receiver::{Answer, Config, Connection, Event};

const HANDSHAKE_TIMEOUT: Duration = Duration::from_secs(10);
/// A connection with no traffic for this long, and no prompt up, is closed.
const IDLE_TIMEOUT: Duration = Duration::from_secs(60);
/// Most connections from one address at a time.
const MAX_PER_PEER: usize = 4;
/// An upload must bring at least this much in every `RATE_WINDOW`, about
/// 1 KiB/s, or it's ended: a trickle would otherwise hold it open for good.
const MIN_UPLOAD_BYTES: u64 = 32 << 10;
const RATE_WINDOW: Duration = Duration::from_secs(30);

#[derive(Debug, Clone)]
pub struct Options {
    pub config: Config,
    /// Where received files go.
    pub destination: PathBuf,
    pub max_connections: usize,
    /// Emit extracted-file byte progress (opt in to preserve existing clients).
    pub report_progress: bool,
}

impl Options {
    pub fn new(config: Config, destination: PathBuf) -> Self {
        Options {
            config,
            destination,
            max_connections: 16,
            report_progress: false,
        }
    }
}

/// An `/Ask` waiting for the user. Dropping it declines.
#[derive(Debug)]
pub struct Request {
    pub id: u64,
    pub peer: SocketAddrV6,
    pub ask: AskRequest,
    reply: oneshot::Sender<Answer>,
}

impl Request {
    pub fn accept(self) {
        let _ = self.reply.send(Answer::Accept);
    }

    pub fn decline(self) {
        let _ = self.reply.send(Answer::Decline);
    }
}

/// What happened, for the frontend. `id` ties a transfer's events together.
#[derive(Debug)]
pub enum ReceiverEvent {
    Discovered {
        peer: SocketAddrV6,
        request: DiscoverRequest,
    },
    Request(Request),
    /// The sender left, or the prompt timed out, before an answer.
    Withdrawn {
        id: u64,
    },
    Urls {
        id: u64,
        urls: Vec<String>,
    },
    /// Actual extracted file bytes written; no compressed-size total implied.
    Progress {
        id: u64,
        done: u64,
    },
    Received {
        id: u64,
        paths: Vec<PathBuf>,
    },
    Failed {
        id: u64,
        reason: String,
    },
}

pub struct Receiver {
    listener: TcpListener,
    acceptor: TlsAcceptor,
    options: Arc<Options>,
    config: Arc<Config>,
    events: mpsc::Sender<ReceiverEvent>,
    ids: Arc<AtomicU64>,
    slots: Arc<Mutex<Slots>>,
    stop: Arc<AtomicBool>,
    paused: Arc<AtomicBool>,
}

/// The connections being served. When every slot is taken, a new connection
/// takes the place of the one that has gone longest without finishing a
/// request, unless that one has an `/Ask` open, so connections that trickle
/// or say nothing can't lock real senders out (SEC-10).
#[derive(Default)]
struct Slots {
    next: u64,
    held: HashMap<u64, Slot>,
}

struct Slot {
    peer: Ipv6Addr,
    /// When the connection opened or last answered a request.
    progress: Instant,
    /// An `/Ask` is open on it: prompting, or accepted and not yet uploaded.
    busy: bool,
    abort: Option<AbortHandle>,
    transfer: Option<u64>,
    committing: bool,
    cancel: Arc<AtomicBool>,
    finished: Arc<AtomicBool>,
}

impl Slots {
    /// A slot for a connection from `peer`, evicting another if all are
    /// taken, or `None` if there's no room.
    fn admit(&mut self, peer: Ipv6Addr, max: usize) -> Option<u64> {
        if self.held.values().filter(|s| s.peer == peer).count() >= MAX_PER_PEER {
            return None;
        }
        if self.held.len() >= max {
            let (&key, _) = self
                .held
                .iter()
                .filter(|(_, s)| !s.busy)
                .min_by_key(|(_, s)| s.progress)?;
            let slot = self.held.remove(&key).unwrap();
            tracing::debug!(peer = %slot.peer, "closing a quiet connection to make room");
            if let Some(abort) = slot.abort {
                abort.abort();
            }
        }
        let key = self.next;
        self.next += 1;
        self.held.insert(
            key,
            Slot {
                peer,
                progress: Instant::now(),
                busy: false,
                abort: None,
                transfer: None,
                committing: false,
                cancel: Arc::default(),
                finished: Arc::default(),
            },
        );
        Some(key)
    }
}

/// A connection's slot, given back when its task ends or is aborted.
struct Held {
    slots: Arc<Mutex<Slots>>,
    key: u64,
    events: mpsc::Sender<ReceiverEvent>,
}

impl Held {
    fn update(&self, f: impl FnOnce(&mut Slot)) {
        if let Some(slot) = self.slots.lock().unwrap().held.get_mut(&self.key) {
            f(slot);
        }
    }
}

impl Drop for Held {
    fn drop(&mut self) {
        if let Some(slot) = self.slots.lock().unwrap().held.remove(&self.key) {
            if let Some(id) = slot.transfer {
                let _ = self.events.try_send(ReceiverEvent::Failed {
                    id,
                    reason: "Connection ended before transfer completed".into(),
                });
            }
            slot.finished.store(true, Ordering::Release);
        }
    }
}

/// Control operations do not detach connection tasks. Cancelling waits until
/// the connection and its staging work have ended. A final commit already in
/// progress completes normally rather than abandoning a blocking filesystem task.
#[derive(Clone)]
pub struct ReceiverControl {
    slots: Arc<Mutex<Slots>>,
    stop: Arc<AtomicBool>,
    paused: Arc<AtomicBool>,
}
impl ReceiverControl {
    pub fn pause(&self, paused: bool) {
        self.paused.store(paused, Ordering::Release);
    }
    pub fn shutdown(&self) {
        self.stop.store(true, Ordering::Release);
    }
    pub async fn cancel(&self, id: u64) -> bool {
        let target = self
            .slots
            .lock()
            .unwrap()
            .held
            .values()
            .find(|slot| slot.transfer == Some(id) && !slot.committing)
            .map(|slot| (slot.cancel.clone(), slot.finished.clone()));
        let Some((cancel, finished)) = target else {
            return false;
        };
        cancel.store(true, Ordering::Release);
        while !finished.load(Ordering::Acquire) {
            tokio::time::sleep(Duration::from_millis(5)).await;
        }
        true
    }
}

impl Receiver {
    /// Listens on `addr`, a link-local address with its scope (NET-2).
    pub async fn bind(
        addr: SocketAddrV6,
        identity: &Identity,
        options: Options,
    ) -> io::Result<(Self, mpsc::Receiver<ReceiverEvent>)> {
        let listener = TcpListener::bind(addr).await?;
        let acceptor = TlsAcceptor::from(tls::server_config(identity)?);
        let (events, rx) = mpsc::channel(64);
        let config = Arc::new(options.config.clone());
        Ok((
            Receiver {
                listener,
                acceptor,
                options: Arc::new(options),
                config,
                events,
                ids: Arc::new(AtomicU64::new(1)),
                slots: Arc::default(),
                stop: Arc::default(),
                paused: Arc::default(),
            },
            rx,
        ))
    }

    pub fn local_addr(&self) -> io::Result<SocketAddrV6> {
        match self.listener.local_addr()? {
            std::net::SocketAddr::V6(addr) => Ok(addr),
            std::net::SocketAddr::V4(_) => Err(io::Error::other("not IPv6")),
        }
    }

    pub fn control(&self) -> ReceiverControl {
        ReceiverControl {
            slots: self.slots.clone(),
            stop: self.stop.clone(),
            paused: self.paused.clone(),
        }
    }

    /// Serves until the event receiver is dropped. A failed connection never
    /// stops the server (SEC-8).
    pub async fn run(self) {
        let mut tasks = JoinSet::new();
        loop {
            let accepted = tokio::select! {
                accepted = self.listener.accept() => accepted,
                () = self.events.closed() => break,
                _ = tokio::time::sleep(Duration::from_millis(25)) => {
                    if self.stop.load(Ordering::Acquire) { break; }
                    while tasks.try_join_next().is_some() {}
                    continue;
                },
            };
            let Ok((stream, peer)) = accepted else {
                // Out of descriptors, say: back off rather than spin.
                tokio::time::sleep(Duration::from_millis(100)).await;
                continue;
            };
            if self.paused.load(Ordering::Acquire) {
                continue;
            }
            let std::net::SocketAddr::V6(peer) = peer else {
                continue;
            };
            let max = self.options.max_connections;
            let Some(key) = self.slots.lock().unwrap().admit(*peer.ip(), max) else {
                continue;
            };
            let held = Held {
                slots: self.slots.clone(),
                key,
                events: self.events.clone(),
            };
            let task = Task {
                acceptor: self.acceptor.clone(),
                options: self.options.clone(),
                config: self.config.clone(),
                events: self.events.clone(),
                ids: self.ids.clone(),
                peer,
            };
            let handle = tasks.spawn(async move {
                if let Err(e) = task.run(stream, &held).await {
                    tracing::debug!(%peer, "connection ended: {e}");
                }
                let unfinished = held
                    .slots
                    .lock()
                    .unwrap()
                    .held
                    .get(&held.key)
                    .and_then(|s| s.transfer);
                if let Some(id) = unfinished {
                    let _ = held
                        .events
                        .send(ReceiverEvent::Failed {
                            id,
                            reason: "Connection ended before transfer completed".into(),
                        })
                        .await;
                    held.update(|s| s.transfer = None);
                }
            });
            let abort = handle;
            if let Some(slot) = self.slots.lock().unwrap().held.get_mut(&key) {
                slot.abort = Some(abort);
            }
        }
        for slot in self.slots.lock().unwrap().held.values() {
            slot.cancel.store(true, Ordering::Release);
        }
        while tasks.join_next().await.is_some() {}
    }
}

/// The request or status line, for the debug log. The peer's bytes, so
/// escaped (SEC-4).
fn first_line(bytes: &[u8]) -> String {
    let line = bytes
        .split(|&b| b == b'\r' || b == b'\n')
        .next()
        .unwrap_or_default();
    format!(
        "{:?}",
        String::from_utf8_lossy(&line[..line.len().min(200)])
    )
}

struct Task {
    acceptor: TlsAcceptor,
    options: Arc<Options>,
    config: Arc<Config>,
    events: mpsc::Sender<ReceiverEvent>,
    ids: Arc<AtomicU64>,
    peer: SocketAddrV6,
}

impl Task {
    async fn run(self, stream: TcpStream, held: &Held) -> io::Result<()> {
        stream.set_nodelay(true)?;
        let mut tls = timeout(HANDSHAKE_TIMEOUT, self.acceptor.accept(stream))
            .await
            .map_err(|_| io::Error::from(io::ErrorKind::TimedOut))??;
        let mut conn = Connection::new(self.config.clone());
        let mut buf = vec![0; 64 << 10];
        let mut timer: Option<Instant> = None;
        let mut idle = Instant::now() + IDLE_TIMEOUT;
        let mut answer: Option<oneshot::Receiver<Answer>> = None;
        let mut staging: Option<Staging> = None;
        let mut id = 0;
        let mut eof = false;
        let mut written = 0u64;
        let cancelled = { held.slots.lock().unwrap().held[&held.key].cancel.clone() };
        // Upload bytes in the current rate window, and when it ends.
        let mut window = (0u64, Instant::now() + RATE_WINDOW);
        loop {
            while let Some(event) = conn.poll() {
                if cancelled.load(Ordering::Acquire) {
                    return Err(io::ErrorKind::Interrupted.into());
                }
                match event {
                    Event::Transmit(bytes) => {
                        if bytes.starts_with(b"HTTP/") {
                            tracing::debug!(peer = %self.peer, "-> {}", first_line(&bytes));
                            held.update(|s| s.progress = Instant::now());
                        }
                        // Flushed: tokio-rustls may hold a record back
                        // when the socket is busy, and the peer would wait
                        // on it for good.
                        timeout(Duration::from_secs(30), async {
                            tls.write_all(&bytes).await?;
                            tls.flush().await
                        })
                        .await
                        .map_err(|_| io::Error::from(io::ErrorKind::TimedOut))??;
                    }
                    Event::SetTimer(after) => timer = after.map(|d| Instant::now() + d),
                    Event::Discovered(request) => {
                        let _ = self.events.try_send(ReceiverEvent::Discovered {
                            peer: self.peer,
                            request,
                        });
                    }
                    Event::Ask(ask) => {
                        tracing::debug!(
                            peer = %self.peer,
                            transfer_type = ?ask.transfer_type,
                            "/Ask {}",
                            ask.described
                        );
                        if ask.transfer_type.as_deref() == Some("links")
                            && matches!(ask.offer, Offer::Files(_))
                        {
                            tracing::warn!(
                                peer = %self.peer,
                                "a link offered as a file: Apple senders don't upload it to a \
                                 receiver without SUPPORTS_URL (MDNS-5)"
                            );
                        }
                        id = self.ids.fetch_add(1, Ordering::Relaxed);
                        held.update(|s| {
                            s.busy = true;
                            s.transfer = Some(id);
                            s.committing = false;
                        });
                        written = 0;
                        let (reply, rx) = oneshot::channel();
                        answer = Some(rx);
                        let request = Request {
                            id,
                            peer: self.peer,
                            ask,
                            reply,
                        };
                        // Nobody listening, or too busy: the prompt goes
                        // unanswered and times out as a decline.
                        let _ = self.events.try_send(ReceiverEvent::Request(request));
                    }
                    Event::AskWithdrawn => {
                        answer = None;
                        held.update(|s| s.transfer = None);
                        held.update(|s| s.busy = false);
                        let _ = self.events.try_send(ReceiverEvent::Withdrawn { id });
                    }
                    Event::Urls(urls) => {
                        let _ = self.events.send(ReceiverEvent::Urls { id, urls }).await;
                        held.update(|s| {
                            s.transfer = None;
                            s.busy = false;
                        });
                    }
                    Event::UploadStarted => match Staging::new(&self.options.destination).await {
                        Ok(s) => {
                            staging = Some(s);
                            window = (0, Instant::now() + RATE_WINDOW);
                        }
                        Err(e) => return self.fail(&mut tls, id, e, held).await,
                    },
                    Event::CreateDir(path) => {
                        if let Err(e) = staging.as_mut().unwrap().create_dir(&path).await {
                            return self.fail(&mut tls, id, e, held).await;
                        }
                    }
                    Event::CreateFile(path) => {
                        if let Err(e) = staging.as_mut().unwrap().create_file(&path).await {
                            return self.fail(&mut tls, id, e, held).await;
                        }
                    }
                    Event::Data(data) => {
                        if let Err(e) = staging.as_mut().unwrap().write(&data).await {
                            return self.fail(&mut tls, id, e, held).await;
                        }
                        written += data.len() as u64;
                        if self.options.report_progress {
                            let _ = self
                                .events
                                .try_send(ReceiverEvent::Progress { id, done: written });
                        }
                    }
                    Event::EndFile => {
                        if let Err(e) = staging.as_mut().unwrap().end_file().await {
                            return self.fail(&mut tls, id, e, held).await;
                        }
                    }
                    Event::UploadComplete => {
                        held.update(|s| s.committing = true);
                        held.update(|s| s.busy = false);
                        let result = staging.take().unwrap().finish().await;
                        conn.finish_upload(result.as_ref().map(|_| ()).map_err(|e| e.to_string()));
                        let event = match result {
                            Ok(paths) => ReceiverEvent::Received { id, paths },
                            Err(e) => ReceiverEvent::Failed {
                                id,
                                reason: e.to_string(),
                            },
                        };
                        let _ = self.events.send(event).await;
                        held.update(|s| s.transfer = None);
                    }
                    Event::UploadFailed(reason) => {
                        // Dropping staging removes it and everything in it.
                        staging = None;
                        held.update(|s| s.busy = false);
                        let _ = self.events.send(ReceiverEvent::Failed { id, reason }).await;
                        held.update(|s| s.transfer = None);
                    }
                    Event::Close => {
                        let _ = tls.shutdown().await;
                        return Ok(());
                    }
                }
            }
            let prompting = answer.is_some();
            tokio::select! {
                _ = tokio::time::sleep(Duration::from_millis(25)) => {
                    if cancelled.load(Ordering::Acquire) { return Err(io::ErrorKind::Interrupted.into()); }
                },
                read = tls.read(&mut buf), if !eof => {
                    match read {
                        Ok(0) => {
                            tracing::debug!(peer = %self.peer, "<- end of stream");
                            eof = true;
                            conn.receive_eof();
                        }
                        Ok(n) => {
                            if buf[..n].starts_with(b"POST ") || buf[..n].starts_with(b"GET ") {
                                tracing::debug!(peer = %self.peer, "<- {}", first_line(&buf[..n]));
                            }
                            let now = Instant::now();
                            idle = now + IDLE_TIMEOUT;
                            if staging.is_some() {
                                window.0 += n as u64;
                                if now >= window.1 {
                                    if window.0 < MIN_UPLOAD_BYTES {
                                        let e = io::Error::new(io::ErrorKind::TimedOut, "upload too slow");
                                        return self.fail(&mut tls, id, e, held).await;
                                    }
                                    window = (0, now + RATE_WINDOW);
                                }
                            }
                            conn.receive(&buf[..n]);
                        }
                        // A reset or TLS error ends the connection, and with
                        // it any prompt or upload.
                        Err(e) => {
                            if prompting {
                                let _ = self.events.try_send(ReceiverEvent::Withdrawn { id });
                                held.update(|s| s.transfer = None);
                            }
                            if staging.is_some() {
                                let _ = self.events.send(ReceiverEvent::Failed { id, reason: e.to_string() }).await;
                                held.update(|s| s.transfer = None);
                            }
                            return Err(e);
                        }
                    }
                }
                () = sleep_until(timer.unwrap_or(idle)), if timer.is_some() => {
                    timer = None;
                    if answer.take().is_some() {
                        held.update(|s| s.busy = false);
                        let _ = self.events.try_send(ReceiverEvent::Withdrawn { id });
                        held.update(|s| s.transfer = None);
                    }
                    conn.timeout();
                }
                () = sleep_until(idle), if !prompting && timer.is_none() => {
                    return Err(io::ErrorKind::TimedOut.into());
                }
                reply = async { answer.as_mut().unwrap().await }, if prompting => {
                    answer = None;
                    // A dropped request is a decline.
                    let reply = reply.unwrap_or(Answer::Decline);
                    if reply == Answer::Decline {
                        held.update(|s| { s.busy = false; s.transfer = None; });
                    }
                    conn.answer(reply);
                }
            }
        }
    }

    async fn fail<S: AsyncWriteExt + Unpin>(
        &self,
        tls: &mut S,
        id: u64,
        e: io::Error,
        held: &Held,
    ) -> io::Result<()> {
        let _ = tls
            .write_all(&crate::proto::http::response(
                500,
                &[("Connection", "close")],
                b"",
            ))
            .await;
        let _ = tls.flush().await;
        let _ = tls.shutdown().await;
        let _ = self
            .events
            .send(ReceiverEvent::Failed {
                id,
                reason: e.to_string(),
            })
            .await;
        held.update(|s| s.transfer = None);
        Err(e)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn cancellation_waits_for_connection_cleanup_and_preserves_commit() {
        let slots = Arc::new(Mutex::new(Slots::default()));
        let key = slots.lock().unwrap().admit(Ipv6Addr::LOCALHOST, 1).unwrap();
        let (cancel, finished) = {
            let mut slots = slots.lock().unwrap();
            let slot = slots.held.get_mut(&key).unwrap();
            slot.transfer = Some(77);
            (slot.cancel.clone(), slot.finished.clone())
        };
        let control = ReceiverControl {
            slots: slots.clone(),
            stop: Arc::default(),
            paused: Arc::default(),
        };
        let cleanup = tokio::spawn(async move {
            while !cancel.load(Ordering::Acquire) {
                tokio::task::yield_now().await;
            }
            tokio::time::sleep(Duration::from_millis(10)).await;
            finished.store(true, Ordering::Release);
        });
        assert!(control.cancel(77).await);
        cleanup.await.unwrap();
        slots.lock().unwrap().held.get_mut(&key).unwrap().committing = true;
        assert!(
            !control.cancel(77).await,
            "do not cancel a filesystem commit already in progress"
        );
        assert!(!control.cancel(99).await);
    }

    fn addr(n: u16) -> Ipv6Addr {
        Ipv6Addr::new(0xfe80, 0, 0, 0, 0, 0, 0, n)
    }

    // spec: SEC-10
    #[test]
    fn a_new_connection_replaces_the_quietest_idle_one() {
        let mut slots = Slots::default();
        let keys: Vec<u64> = (1..=3).map(|n| slots.admit(addr(n), 3).unwrap()).collect();
        // The oldest is prompting; of the others, the second answered a
        // request last.
        slots.held.get_mut(&keys[0]).unwrap().busy = true;
        slots.held.get_mut(&keys[1]).unwrap().progress += Duration::from_secs(5);
        let new = slots.admit(addr(4), 3).unwrap();
        let mut left: Vec<u64> = slots.held.keys().copied().collect();
        left.sort();
        assert_eq!(left, [keys[0], keys[1], new]);
    }

    // spec: SEC-10
    #[test]
    fn busy_connections_are_never_replaced() {
        let mut slots = Slots::default();
        for n in 1..=2 {
            let key = slots.admit(addr(n), 2).unwrap();
            slots.held.get_mut(&key).unwrap().busy = true;
        }
        assert_eq!(slots.admit(addr(3), 2), None);
        assert_eq!(slots.held.len(), 2);
    }

    // spec: SEC-10
    #[test]
    fn one_address_gets_a_few_connections() {
        let mut slots = Slots::default();
        for _ in 0..MAX_PER_PEER {
            assert!(slots.admit(addr(1), 16).is_some());
        }
        assert_eq!(slots.admit(addr(1), 16), None);
        assert!(slots.admit(addr(2), 16).is_some());
    }
}
