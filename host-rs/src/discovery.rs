use std::collections::BTreeMap;
use std::io;
use std::net::{IpAddr, Ipv4Addr, SocketAddr, UdpSocket};
use std::time::{Duration, Instant};

pub const DEFAULT_WIFI_PORT: u16 = 29_380;
pub const DEFAULT_DISCOVERY_TIMEOUT: Duration = Duration::from_secs(2);
pub const DISCOVERY_PROBE_INTERVAL: Duration = Duration::from_millis(250);
pub const PROTOCOL_PREFIX: &str = "KNIETTY/1";
const DISCOVERY_PROBE: &[u8] = b"KNIETTY/1 DISCOVER\n";

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct NetworkDevice {
    pub name: String,
    pub address: IpAddr,
    pub port: u16,
    pub device_id: String,
    pub tls_required: bool,
}

pub fn parse_discovery_response(response: &[u8], address: IpAddr) -> Option<NetworkDevice> {
    let response = std::str::from_utf8(response).ok()?;
    if !response.is_ascii() {
        return None;
    }
    let fields: Vec<_> = response.split_whitespace().collect();
    if !matches!(fields.len(), 4 | 5)
        || fields[0] != PROTOCOL_PREFIX
        || fields[1] != "HERE"
        || (fields.len() == 5 && fields[4] != "tls=required")
    {
        return None;
    }
    let port = fields[3].parse::<u16>().ok()?;
    if port == 0 {
        return None;
    }
    Some(NetworkDevice {
        name: fields[2].to_owned(),
        address,
        port,
        device_id: fields[2].to_owned(),
        tls_required: fields.get(4) == Some(&"tls=required"),
    })
}

pub fn discover_network_devices(timeout: Duration, port: u16) -> io::Result<Vec<NetworkDevice>> {
    discover_network_devices_at(timeout, SocketAddr::from((Ipv4Addr::BROADCAST, port)))
}

fn discover_network_devices_at(
    timeout: Duration,
    destination: SocketAddr,
) -> io::Result<Vec<NetworkDevice>> {
    let socket = UdpSocket::bind(SocketAddr::from((Ipv4Addr::UNSPECIFIED, 0)))?;
    socket.set_broadcast(true)?;

    let deadline = Instant::now() + timeout;
    let mut next_probe_at = Instant::now();
    let mut devices = BTreeMap::new();
    let mut buffer = [0_u8; 256];

    loop {
        let now = Instant::now();
        if now >= deadline {
            break;
        }
        if now >= next_probe_at {
            socket.send_to(DISCOVERY_PROBE, destination)?;
            next_probe_at = now + DISCOVERY_PROBE_INTERVAL;
        }

        let now = Instant::now();
        let remaining = deadline.saturating_duration_since(now);
        let until_probe = next_probe_at.saturating_duration_since(now);
        let receive_timeout = remaining.min(until_probe).max(Duration::from_millis(1));
        socket.set_read_timeout(Some(receive_timeout))?;

        match socket.recv_from(&mut buffer) {
            Ok((length, source)) => {
                if let Some(device) = parse_discovery_response(&buffer[..length], source.ip()) {
                    devices.insert((device.address, device.port), device);
                }
            }
            Err(error)
                if matches!(
                    error.kind(),
                    io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut
                ) => {}
            Err(error) => return Err(error),
        }
    }

    let mut devices: Vec<_> = devices.into_values().collect();
    devices.sort_by(|left, right| {
        (&left.name, left.address.to_string(), left.port).cmp(&(
            &right.name,
            right.address.to_string(),
            right.port,
        ))
    });
    Ok(devices)
}

pub fn format_network_device(device: &NetworkDevice) -> String {
    format!(
        "{}\t{}\t{}\t{}\t{}",
        device.name,
        device.address,
        device.port,
        device.device_id,
        if device.tls_required {
            "required"
        } else {
            "legacy"
        }
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::thread;

    #[test]
    fn parses_valid_discovery_response() {
        assert_eq!(
            parse_discovery_response(
                b"KNIETTY/1 HERE knietty-aabbcc 29380\n",
                "192.0.2.10".parse().unwrap()
            ),
            Some(NetworkDevice {
                name: "knietty-aabbcc".to_owned(),
                address: "192.0.2.10".parse().unwrap(),
                port: 29_380,
                device_id: "knietty-aabbcc".to_owned(),
                tls_required: false,
            })
        );
    }

    #[test]
    fn rejects_malformed_discovery_responses() {
        let address = "192.0.2.10".parse().unwrap();
        for response in [
            &b"not knietty\n"[..],
            &b"KNIETTY/2 HERE x4 29380\n"[..],
            &b"KNIETTY/1 HERE x4 0\n"[..],
            &b"KNIETTY/1 HERE x4 65536\n"[..],
            &b"KNIETTY/1 HERE x4 nope\n"[..],
            &b"KNIETTY/1 HERE \xff 29380\n"[..],
        ] {
            assert_eq!(parse_discovery_response(response, address), None);
        }
    }

    #[test]
    fn formats_stable_device_rows() {
        let device = NetworkDevice {
            name: "knietty-aabbcc".to_owned(),
            address: "192.0.2.10".parse().unwrap(),
            port: 29_380,
            device_id: "knietty-aabbcc".to_owned(),
            tls_required: true,
        };
        assert_eq!(
            format_network_device(&device),
            "knietty-aabbcc\t192.0.2.10\t29380\tknietty-aabbcc\trequired"
        );
    }

    #[test]
    fn discovery_reprobes_until_a_device_replies() {
        let responder = UdpSocket::bind(SocketAddr::from((Ipv4Addr::LOCALHOST, 0))).unwrap();
        responder
            .set_read_timeout(Some(Duration::from_secs(2)))
            .unwrap();
        let destination = responder.local_addr().unwrap();
        let worker = thread::spawn(move || {
            let mut buffer = [0_u8; 64];
            for probe_number in 0..2 {
                let (length, source) = responder.recv_from(&mut buffer).unwrap();
                assert_eq!(&buffer[..length], DISCOVERY_PROBE);
                if probe_number == 1 {
                    responder
                        .send_to(
                            b"KNIETTY/1 HERE knietty-aabbcc 29380 tls=required\n",
                            source,
                        )
                        .unwrap();
                }
            }
        });

        let devices = discover_network_devices_at(Duration::from_millis(700), destination).unwrap();
        worker.join().unwrap();
        assert_eq!(
            devices,
            vec![NetworkDevice {
                name: "knietty-aabbcc".to_owned(),
                address: IpAddr::V4(Ipv4Addr::LOCALHOST),
                port: 29_380,
                device_id: "knietty-aabbcc".to_owned(),
                tls_required: true,
            }]
        );
    }
}
