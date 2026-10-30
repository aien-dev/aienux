//! Loopback-only model fixture for ck_courier_udp_test.sh. No external runtime.
use std::io::{self, Read, Write};
use std::net::{TcpListener, TcpStream, UdpSocket};
use std::time::Duration;

fn request(stream: &mut TcpStream) -> io::Result<String> {
    stream.set_read_timeout(Some(Duration::from_secs(10)))?;
    let mut bytes = Vec::new();
    let mut one = [0u8; 1];
    while !bytes.ends_with(b"\r\n\r\n") {
        stream.read_exact(&mut one)?;
        bytes.push(one[0]);
        if bytes.len() > 8192 {
            return Err(io::Error::other("oversized request header"));
        }
    }
    let head = String::from_utf8_lossy(&bytes);
    let length = head
        .lines()
        .filter_map(|line| line.split_once(':'))
        .find(|(name, _)| name.eq_ignore_ascii_case("content-length"))
        .and_then(|(_, value)| value.trim().parse::<usize>().ok())
        .filter(|&length| length <= 65536)
        .ok_or_else(|| io::Error::other("missing or oversized request body"))?;
    let mut body = vec![0; length];
    stream.read_exact(&mut body)?;
    String::from_utf8(body).map_err(io::Error::other)
}

fn main() -> io::Result<()> {
    let args: Vec<String> = std::env::args().collect();
    if args.get(1).map(String::as_str) == Some("--free-udp-port") {
        let socket = UdpSocket::bind("127.0.0.1:0")?;
        println!("{}", socket.local_addr()?.port());
        return Ok(());
    }
    let busy: usize = args[2].parse().expect("busy request count");
    let delay: u64 = args[3].parse().expect("delay in seconds");
    // Bind before publishing the port so the shell never races server startup.
    let listener = TcpListener::bind("127.0.0.1:0")?;
    std::fs::write(&args[1], listener.local_addr()?.port().to_string())?;
    for (seen, stream) in listener.incoming().enumerate() {
        let mut stream = stream?;
        stream.set_write_timeout(Some(Duration::from_secs(10)))?;
        let body = request(&mut stream)?;
        let system = body
            .strip_prefix("{\"messages\":[{\"role\":\"system\",\"content\":")
            .and_then(|rest| rest.split_once("},{\"role\":\"user\""))
            .map(|(system, _)| system)
            .expect("courier must send its system message first");
        assert!(system.starts_with('"') && system.contains("Allen"));
        if seen < busy {
            stream.write_all(b"HTTP/1.1 503 Service Unavailable\r\nRetry-After: 1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")?;
            continue;
        }
        std::thread::sleep(Duration::from_secs(delay));
        stream.write_all(
            b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n",
        )?;
        for word in "I am here. The rain is soft tonight and I like listening to it with you. Tell me more about your day when you are ready.".split(' ') {
            // Fixture words contain no JSON escapes.
            write!(stream, "data: {{\"choices\":[{{\"delta\":{{\"content\":\"{word} \"}}}}]}}\n\n")?;
            stream.flush()?;
            std::thread::sleep(Duration::from_millis(30));
        }
        stream.write_all(b"data: [DONE]\n\n")?;
    }
    Ok(())
}
