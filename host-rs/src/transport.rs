use std::env;
use std::fmt;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read, Write};
use std::net::TcpStream;
use std::os::fd::{AsFd, BorrowedFd};
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Duration;

use rcgen::{generate_simple_self_signed, CertifiedKey};
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::crypto::{verify_tls12_signature, verify_tls13_signature, WebPkiSupportedAlgorithms};
use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer, ServerName, UnixTime};
use rustls::{ClientConfig, ClientConnection, DigitallySignedStruct, SignatureScheme, StreamOwned};
use sha2::{Digest, Sha256};

const IDENTITY_MAGIC: &[u8; 8] = b"KNIHOST1";
const PAIRING_DOMAIN: &[u8] = b"knietty-pairing-v1";
const MAX_CERTIFICATE_SIZE: usize = 2048;
const MAX_PRIVATE_KEY_SIZE: usize = 1024;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SecurityMode {
    Tls,
    InsecurePlaintext,
}

impl Default for SecurityMode {
    fn default() -> Self {
        Self::Tls
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PairingInfo {
    pub code: String,
    pub first_pairing: bool,
    pub device_fingerprint: String,
    pub host_fingerprint: String,
}

#[derive(Debug)]
struct PinnedServerVerifier {
    expected_fingerprint: Option<[u8; 32]>,
    supported: WebPkiSupportedAlgorithms,
}

impl ServerCertVerifier for PinnedServerVerifier {
    fn verify_server_cert(
        &self,
        end_entity: &CertificateDer<'_>,
        intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        _ocsp_response: &[u8],
        _now: UnixTime,
    ) -> Result<ServerCertVerified, rustls::Error> {
        if !intermediates.is_empty()
            || end_entity.is_empty()
            || end_entity.len() > MAX_CERTIFICATE_SIZE
        {
            return Err(rustls::Error::General(
                "knietty requires one bounded device leaf certificate".to_owned(),
            ));
        }
        if let Some(expected) = self.expected_fingerprint {
            let actual = sha256(end_entity.as_ref());
            if actual != expected {
                return Err(rustls::Error::General(
                    "X4 TLS identity changed; forget the old pairing before reconnecting"
                        .to_owned(),
                ));
            }
        }
        Ok(ServerCertVerified::assertion())
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        certificate: &CertificateDer<'_>,
        signature: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        verify_tls12_signature(message, certificate, signature, &self.supported)
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        certificate: &CertificateDer<'_>,
        signature: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        verify_tls13_signature(message, certificate, signature, &self.supported)
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        self.supported.supported_schemes()
    }
}

#[derive(Clone)]
struct HostIdentity {
    certificate: Vec<u8>,
    private_key: Vec<u8>,
}

enum StreamKind {
    Plain(TcpStream),
    Tls(Box<StreamOwned<ClientConnection, TcpStream>>),
}

pub struct TerminalStream {
    stream: StreamKind,
    pairing: Option<PairingInfo>,
    peer_certificate: Option<Vec<u8>>,
    pin_path: Option<PathBuf>,
}

impl fmt::Debug for TerminalStream {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("TerminalStream")
            .field(
                "security",
                &match self.stream {
                    StreamKind::Plain(_) => "insecure-plaintext",
                    StreamKind::Tls(_) => "tls1.3",
                },
            )
            .field("pairing", &self.pairing)
            .finish_non_exhaustive()
    }
}

impl TerminalStream {
    pub fn plain(stream: TcpStream) -> Self {
        Self {
            stream: StreamKind::Plain(stream),
            pairing: None,
            peer_certificate: None,
            pin_path: None,
        }
    }

    pub fn connect(
        mut stream: TcpStream,
        device_id: &str,
        mode: SecurityMode,
        timeout: Duration,
    ) -> io::Result<Self> {
        if mode == SecurityMode::InsecurePlaintext {
            return Ok(Self::plain(stream));
        }
        stream.set_read_timeout(Some(timeout))?;
        stream.set_write_timeout(Some(timeout))?;

        let directory = config_directory()?;
        ensure_private_directory(&directory)?;
        let identity = load_or_create_identity(&directory)?;
        let pins = directory.join("devices");
        ensure_private_directory(&pins)?;
        let pin_path = pins.join(format!("{}.der", safe_device_id(device_id)));
        let pinned_certificate = load_bounded_file(&pin_path, MAX_CERTIFICATE_SIZE)?;
        let expected_fingerprint = pinned_certificate.as_deref().map(sha256);

        let provider = rustls::crypto::ring::default_provider();
        let verifier = PinnedServerVerifier {
            expected_fingerprint,
            supported: provider.signature_verification_algorithms,
        };
        let mut config = ClientConfig::builder_with_provider(Arc::new(provider))
            .with_protocol_versions(&[&rustls::version::TLS13])
            .map_err(tls_io)?
            .dangerous()
            .with_custom_certificate_verifier(Arc::new(verifier))
            .with_client_auth_cert(
                vec![CertificateDer::from(identity.certificate.clone())],
                PrivateKeyDer::from(PrivatePkcs8KeyDer::from(identity.private_key.clone())),
            )
            .map_err(tls_io)?;
        config.max_fragment_size = Some(2048);

        let server_name = ServerName::try_from("knietty.local".to_owned())
            .map_err(|error| io::Error::new(io::ErrorKind::InvalidInput, error))?;
        let mut connection =
            ClientConnection::new(Arc::new(config), server_name).map_err(tls_io)?;
        while connection.is_handshaking() {
            connection.complete_io(&mut stream).map_err(tls_io)?;
        }
        let peer_certificate = connection
            .peer_certificates()
            .and_then(|chain| chain.first())
            .map(|certificate| certificate.as_ref().to_vec())
            .ok_or_else(|| {
                io::Error::new(io::ErrorKind::InvalidData, "X4 sent no TLS certificate")
            })?;
        if peer_certificate.len() > MAX_CERTIFICATE_SIZE {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "X4 TLS certificate exceeds the host bound",
            ));
        }
        let device_fingerprint = sha256(&peer_certificate);
        let host_fingerprint = sha256(&identity.certificate);
        let pairing = PairingInfo {
            code: pairing_code(&device_fingerprint, &host_fingerprint),
            first_pairing: pinned_certificate.is_none(),
            device_fingerprint: fingerprint_text(&device_fingerprint),
            host_fingerprint: fingerprint_text(&host_fingerprint),
        };

        stream.set_read_timeout(None)?;
        stream.set_write_timeout(None)?;
        Ok(Self {
            stream: StreamKind::Tls(Box::new(StreamOwned::new(connection, stream))),
            pairing: Some(pairing),
            peer_certificate: Some(peer_certificate),
            pin_path: Some(pin_path),
        })
    }

    pub fn pairing(&self) -> Option<&PairingInfo> {
        self.pairing.as_ref()
    }

    pub fn confirm_pairing(&self) -> io::Result<()> {
        let Some(pairing) = &self.pairing else {
            return Ok(());
        };
        if !pairing.first_pairing {
            return Ok(());
        }
        let certificate = self.peer_certificate.as_deref().ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::InvalidData,
                "TLS peer certificate is unavailable",
            )
        })?;
        let path = self.pin_path.as_deref().ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::InvalidData,
                "TLS device pin path is unavailable",
            )
        })?;
        atomic_private_write(path, certificate)
    }

    pub fn tcp(&self) -> &TcpStream {
        match &self.stream {
            StreamKind::Plain(stream) => stream,
            StreamKind::Tls(stream) => stream.get_ref(),
        }
    }

    pub fn set_nonblocking(&self, enabled: bool) -> io::Result<()> {
        self.tcp().set_nonblocking(enabled)
    }

    pub fn set_write_timeout(&self, timeout: Option<Duration>) -> io::Result<()> {
        self.tcp().set_write_timeout(timeout)
    }

    pub fn set_read_timeout(&self, timeout: Option<Duration>) -> io::Result<()> {
        self.tcp().set_read_timeout(timeout)
    }
}

impl Read for TerminalStream {
    fn read(&mut self, output: &mut [u8]) -> io::Result<usize> {
        match &mut self.stream {
            StreamKind::Plain(stream) => stream.read(output),
            StreamKind::Tls(stream) => stream.read(output),
        }
    }
}

impl Write for TerminalStream {
    fn write(&mut self, input: &[u8]) -> io::Result<usize> {
        match &mut self.stream {
            StreamKind::Plain(stream) => stream.write(input),
            StreamKind::Tls(stream) => stream.write(input),
        }
    }

    fn flush(&mut self) -> io::Result<()> {
        match &mut self.stream {
            StreamKind::Plain(stream) => stream.flush(),
            StreamKind::Tls(stream) => stream.flush(),
        }
    }
}

impl AsFd for TerminalStream {
    fn as_fd(&self) -> BorrowedFd<'_> {
        self.tcp().as_fd()
    }
}

fn tls_io(error: impl fmt::Display) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, format!("TLS: {error}"))
}

fn sha256(bytes: &[u8]) -> [u8; 32] {
    Sha256::digest(bytes).into()
}

fn pairing_code(device: &[u8; 32], host: &[u8; 32]) -> String {
    let mut digest = Sha256::new();
    digest.update(PAIRING_DOMAIN);
    digest.update(device);
    digest.update(host);
    let digest = digest.finalize();
    let value =
        u32::from_be_bytes(digest[..4].try_into().expect("SHA-256 has four bytes")) % 1_000_000;
    format!("{value:06}")
}

fn fingerprint_text(fingerprint: &[u8; 32]) -> String {
    fingerprint[..8]
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect::<Vec<_>>()
        .join(":")
}

fn safe_device_id(device_id: &str) -> String {
    let sanitized: String = device_id
        .chars()
        .map(|character| {
            if character.is_ascii_alphanumeric() || matches!(character, '-' | '_' | '.') {
                character
            } else {
                '_'
            }
        })
        .take(64)
        .collect();
    if sanitized.is_empty() {
        "x4".to_owned()
    } else {
        sanitized
    }
}

fn config_directory() -> io::Result<PathBuf> {
    if let Some(path) = env::var_os("KNIETTY_CONFIG_DIR").filter(|path| !path.is_empty()) {
        return Ok(PathBuf::from(path));
    }
    let home = env::var_os("HOME")
        .filter(|path| !path.is_empty())
        .ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::NotFound,
                "HOME is unavailable for TLS identity storage",
            )
        })?;
    if cfg!(target_os = "macos") {
        Ok(PathBuf::from(home).join("Library/Application Support/knietty"))
    } else if let Some(path) = env::var_os("XDG_CONFIG_HOME").filter(|path| !path.is_empty()) {
        Ok(PathBuf::from(path).join("knietty"))
    } else {
        Ok(PathBuf::from(home).join(".config/knietty"))
    }
}

fn ensure_private_directory(path: &Path) -> io::Result<()> {
    fs::create_dir_all(path)?;
    fs::set_permissions(path, fs::Permissions::from_mode(0o700))
}

fn load_bounded_file(path: &Path, limit: usize) -> io::Result<Option<Vec<u8>>> {
    let metadata = match fs::metadata(path) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(None),
        Err(error) => return Err(error),
    };
    if !metadata.is_file() || metadata.len() == 0 || metadata.len() > limit as u64 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("invalid bounded TLS state file {}", path.display()),
        ));
    }
    let bytes = fs::read(path)?;
    if bytes.len() > limit {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "TLS state grew while reading",
        ));
    }
    Ok(Some(bytes))
}

fn load_or_create_identity(directory: &Path) -> io::Result<HostIdentity> {
    let path = directory.join("identity.bin");
    match load_bounded_file(
        &path,
        IDENTITY_MAGIC.len() + 8 + MAX_CERTIFICATE_SIZE + MAX_PRIVATE_KEY_SIZE,
    )? {
        Some(bytes) => decode_identity(&bytes),
        None => {
            let CertifiedKey { cert, key_pair } =
                generate_simple_self_signed(vec!["knietty-host".to_owned()]).map_err(tls_io)?;
            let identity = HostIdentity {
                certificate: cert.der().as_ref().to_vec(),
                private_key: key_pair.serialize_der(),
            };
            atomic_private_write(&path, &encode_identity(&identity)?)?;
            Ok(identity)
        }
    }
}

fn encode_identity(identity: &HostIdentity) -> io::Result<Vec<u8>> {
    if identity.certificate.is_empty()
        || identity.certificate.len() > MAX_CERTIFICATE_SIZE
        || identity.private_key.is_empty()
        || identity.private_key.len() > MAX_PRIVATE_KEY_SIZE
    {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "generated TLS identity exceeds bounds",
        ));
    }
    let mut output = Vec::with_capacity(
        IDENTITY_MAGIC.len() + 8 + identity.certificate.len() + identity.private_key.len(),
    );
    output.extend_from_slice(IDENTITY_MAGIC);
    output.extend_from_slice(&(identity.certificate.len() as u32).to_be_bytes());
    output.extend_from_slice(&(identity.private_key.len() as u32).to_be_bytes());
    output.extend_from_slice(&identity.certificate);
    output.extend_from_slice(&identity.private_key);
    Ok(output)
}

fn decode_identity(bytes: &[u8]) -> io::Result<HostIdentity> {
    if bytes.len() < IDENTITY_MAGIC.len() + 8 || &bytes[..IDENTITY_MAGIC.len()] != IDENTITY_MAGIC {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "invalid knietty TLS identity header",
        ));
    }
    let offset = IDENTITY_MAGIC.len();
    let cert_length = u32::from_be_bytes(bytes[offset..offset + 4].try_into().unwrap()) as usize;
    let key_length = u32::from_be_bytes(bytes[offset + 4..offset + 8].try_into().unwrap()) as usize;
    if cert_length == 0
        || cert_length > MAX_CERTIFICATE_SIZE
        || key_length == 0
        || key_length > MAX_PRIVATE_KEY_SIZE
        || bytes.len() != offset + 8 + cert_length + key_length
    {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "invalid bounded knietty TLS identity",
        ));
    }
    Ok(HostIdentity {
        certificate: bytes[offset + 8..offset + 8 + cert_length].to_vec(),
        private_key: bytes[offset + 8 + cert_length..].to_vec(),
    })
}

fn atomic_private_write(path: &Path, bytes: &[u8]) -> io::Result<()> {
    let parent = path
        .parent()
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "TLS path has no parent"))?;
    ensure_private_directory(parent)?;
    let temporary = parent.join(format!(
        ".{}.{}.tmp",
        path.file_name().unwrap_or_default().to_string_lossy(),
        std::process::id()
    ));
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(&temporary)?;
    let result = (|| {
        file.write_all(bytes)?;
        file.sync_all()?;
        fs::rename(&temporary, path)?;
        let directory = File::open(parent)?;
        directory.sync_all()
    })();
    if result.is_err() {
        let _ = fs::remove_file(&temporary);
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};

    fn temporary_directory() -> PathBuf {
        static NEXT: AtomicU64 = AtomicU64::new(1);
        env::temp_dir().join(format!(
            "knietty-tls-test-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ))
    }

    #[test]
    fn identity_encoding_is_bounded_and_round_trips() {
        let identity = HostIdentity {
            certificate: vec![1, 2, 3],
            private_key: vec![4, 5],
        };
        assert_eq!(
            decode_identity(&encode_identity(&identity).unwrap())
                .unwrap()
                .certificate,
            identity.certificate
        );
        assert!(decode_identity(b"bad").is_err());
    }

    #[test]
    fn private_atomic_state_uses_restrictive_permissions() {
        let directory = temporary_directory();
        let path = directory.join("devices/x4.der");
        atomic_private_write(&path, b"certificate").unwrap();
        assert_eq!(fs::read(&path).unwrap(), b"certificate");
        assert_eq!(fs::metadata(&path).unwrap().permissions().mode() & 0o077, 0);
        assert_eq!(
            fs::metadata(path.parent().unwrap())
                .unwrap()
                .permissions()
                .mode()
                & 0o077,
            0
        );
        fs::remove_dir_all(directory).unwrap();
    }

    #[test]
    fn sas_is_role_ordered_and_stable() {
        let device = [1_u8; 32];
        let host = [2_u8; 32];
        assert_eq!(pairing_code(&device, &host), pairing_code(&device, &host));
        assert_ne!(pairing_code(&device, &host), pairing_code(&host, &device));
        assert_eq!(pairing_code(&device, &host).len(), 6);
    }
}
