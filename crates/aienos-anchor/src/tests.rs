use super::*;
use ed25519_dalek::{Signer, SigningKey};

fn key(seed: u8) -> SigningKey {
    SigningKey::from_bytes(&[seed; 32])
}

fn sign(k: &SigningKey, body: Body) -> Entry {
    let signature = k.sign(&body.encode()).to_bytes();
    Entry { body, signer: k.verifying_key().to_bytes(), signature }
}

fn admit(a: &mut Anchor, k: &SigningKey, fact: &[u8], value: &[u8]) -> Result<(), Refusal> {
    let body = a.next_body(Kind::Admit, 1, fact, value, b"test");
    a.append(sign(k, body))
}

fn anchor() -> (Anchor, SigningKey) {
    let k = key(7);
    (Anchor::new(alloc::vec![k.verifying_key().to_bytes()]), k)
}

#[test]
fn admitted_fact_is_known() {
    let (mut a, k) = anchor();
    admit(&mut a, &k, b"aien.name", b"Aien").unwrap();
    match a.query(b"aien.name") {
        Answer::Known { value, seq, .. } => assert_eq!((value, seq), (&b"Aien"[..], 0)),
        other => panic!("{other:?}"),
    }
}

#[test]
fn missing_fact_is_unknown() {
    let (a, _) = anchor();
    assert_eq!(a.query(b"anything"), Answer::Unknown);
}

#[test]
fn newer_fact_wins_and_retraction_is_recorded() {
    let (mut a, k) = anchor();
    admit(&mut a, &k, b"x", b"1").unwrap();
    admit(&mut a, &k, b"x", b"2").unwrap();
    assert!(matches!(a.query(b"x"), Answer::Known { value: b"2", .. }));
    let body = a.next_body(Kind::Retract, 2, b"x", b"", b"wrong");
    a.append(sign(&k, body)).unwrap();
    assert_eq!(a.query(b"x"), Answer::Retracted { seq: 2 });
    assert_eq!(a.len(), 3);
}

#[test]
fn model_key_cannot_write() {
    let (mut a, _) = anchor();
    let model = key(9);
    assert_eq!(admit(&mut a, &model, b"x", b"1"), Err(Refusal::UntrustedSigner));
    assert!(a.is_empty());
}

#[test]
fn tampered_value_is_refused() {
    let (mut a, k) = anchor();
    let mut e = sign(&k, a.next_body(Kind::Admit, 1, b"x", b"true", b"t"));
    e.body.value = b"false".to_vec();
    assert_eq!(a.append(e), Err(Refusal::BadSignature));
}

#[test]
fn out_of_order_and_broken_chain_are_refused() {
    let (mut a, k) = anchor();
    let mut body = a.next_body(Kind::Admit, 1, b"x", b"1", b"t");
    body.seq = 5;
    assert_eq!(a.append(sign(&k, body)), Err(Refusal::OutOfOrder));
    let mut body = a.next_body(Kind::Admit, 1, b"x", b"1", b"t");
    body.prev = [1; 32];
    assert_eq!(a.append(sign(&k, body)), Err(Refusal::BrokenChain));
}

#[test]
fn retract_rules() {
    let (mut a, k) = anchor();
    let body = a.next_body(Kind::Retract, 1, b"never", b"", b"t");
    assert_eq!(a.append(sign(&k, body)), Err(Refusal::RetractUnknown));
    admit(&mut a, &k, b"x", b"1").unwrap();
    let body = a.next_body(Kind::Retract, 1, b"x", b"sneaky", b"t");
    assert_eq!(a.append(sign(&k, body)), Err(Refusal::RetractWithValue));
}

#[test]
fn bounds_are_enforced() {
    let (mut a, k) = anchor();
    assert_eq!(admit(&mut a, &k, &[b'k'; MAX_KEY + 1], b"1"), Err(Refusal::TooLarge));
    assert_eq!(admit(&mut a, &k, b"", b"1"), Err(Refusal::TooLarge));
    assert_eq!(admit(&mut a, &k, b"k", &[0; MAX_VALUE + 1]), Err(Refusal::TooLarge));
}

#[test]
fn export_replays_to_the_same_head() {
    let (mut a, k) = anchor();
    admit(&mut a, &k, b"a", b"1").unwrap();
    admit(&mut a, &k, b"b", b"2").unwrap();
    let b = Anchor::replay(alloc::vec![k.verifying_key().to_bytes()], &a.export()).unwrap();
    assert_eq!((b.head(), b.len()), (a.head(), 2));
    assert!(matches!(b.query(b"b"), Answer::Known { value: b"2", .. }));
}

#[test]
fn tampered_log_is_refused_whole() {
    let (mut a, k) = anchor();
    admit(&mut a, &k, b"a", b"1").unwrap();
    admit(&mut a, &k, b"b", b"2").unwrap();
    let trusted = alloc::vec![k.verifying_key().to_bytes()];
    let mut log = a.export();
    let i = log.iter().position(|&c| c == b'1').unwrap();
    log[i] = b'9';
    assert!(Anchor::replay(trusted.clone(), &log).is_err());
    let log = a.export();
    assert_eq!(Anchor::replay(trusted.clone(), &log[..log.len() - 1]).err(), Some(Refusal::Malformed));
    // Dropping the first entry breaks the chain for the rest.
    let first = Entry::decode(&log).unwrap().1;
    assert_eq!(Anchor::replay(trusted, &log[first..]).err(), Some(Refusal::OutOfOrder));
}

#[test]
fn signature_from_another_domain_is_refused() {
    let (mut a, k) = anchor();
    let body = a.next_body(Kind::Admit, 1, b"x", b"1", b"t");
    let mut msg = body.encode();
    msg[0] = b'X';
    let e = Entry { body, signer: k.verifying_key().to_bytes(), signature: k.sign(&msg).to_bytes() };
    assert_eq!(a.append(e), Err(Refusal::BadSignature));
}
