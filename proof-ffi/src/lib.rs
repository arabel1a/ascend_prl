//! C ABI for PlainProof construction — module 3 of the C miner.
//!
//! Mirrors mps-miner/proof_builder.py: build keyed merkle trees over raw A (m*k)
//! and B (n*k) bytes, take multileaf proofs for the hit rows/cols, serialize the
//! PlainProof with bincode and base64-encode into the caller's buffer.

use blake3::CHUNK_LEN;
use pearl_blake3::MerkleTree;
use zk_pow::ffi::plain_proof::{MatrixMerkleProof, PlainProof};

fn leaf_indices_from_rows(rows: &[usize], cols: usize) -> Vec<usize> {
    MerkleTree::compute_leaf_indices_from_rows(rows, (0, cols))
}

fn matrix_proof(data: &[u8], key: [u8; 32], rows: &[usize], cols: usize) -> MatrixMerkleProof {
    let tree = MerkleTree::new(data, key);
    let leaves = leaf_indices_from_rows(rows, cols);
    let p = tree.get_multileaf_proof(&leaves);
    MatrixMerkleProof::new(
        p.leaf_data,
        p.leaf_indices,
        rows.to_vec(),
        p.total_leaves,
        p.root,
        p.siblings,
    )
}

/// hash key = blake3(incomplete_header || mining_config.to_bytes()) — proof_builder.py.
#[no_mangle]
pub unsafe extern "C" fn hash_key(
    header: *const u8,
    header_len: usize,
    common_dim: usize,
    rank: usize,
    rows_pat: *const usize,
    n_rows: usize,
    cols_pat: *const usize,
    n_cols: usize,
    out: *mut u8,
) -> i32 {
    use zk_pow::api::proof::{MMAType, MiningConfiguration, PeriodicPattern};
    let rows: Vec<u32> = std::slice::from_raw_parts(rows_pat, n_rows)
        .iter().map(|&v| v as u32).collect();
    let cols: Vec<u32> = std::slice::from_raw_parts(cols_pat, n_cols)
        .iter().map(|&v| v as u32).collect();
    let cfg = MiningConfiguration {
        common_dim: common_dim as u32,
        rank: rank as u16,
        mma_type: MMAType::Int7xInt7ToInt32,
        rows_pattern: PeriodicPattern::from_list(&rows).unwrap(),
        cols_pattern: PeriodicPattern::from_list(&cols).unwrap(),
        moe: None,
    };
    let mut h = blake3::Hasher::new();
    h.update(std::slice::from_raw_parts(header, header_len));
    h.update(&cfg.to_bytes());
    std::ptr::copy_nonoverlapping(h.finalize().as_bytes().as_ptr(), out, 32);
    0
}

/// Build a PlainProof and write base64 into out (cap bytes). Returns the b64
/// length, or -1 if cap was too small. A is m*k bytes, B is n*k bytes (raw int8,
/// chunk-aligned shapes only).
#[no_mangle]
pub unsafe extern "C" fn build_proof_b64(
    a: *const u8,
    b: *const u8,
    m: usize,
    n: usize,
    k: usize,
    rank: usize,
    key: *const u8,
    a_rows: *const usize,
    na: usize,
    b_cols: *const usize,
    nb: usize,
    out: *mut u8,
    cap: usize,
) -> isize {
    assert!((m * k) % CHUNK_LEN == 0 && (n * k) % CHUNK_LEN == 0);
    let a = std::slice::from_raw_parts(a, m * k);
    let b = std::slice::from_raw_parts(b, n * k);
    let mut key32 = [0u8; 32];
    key32.copy_from_slice(std::slice::from_raw_parts(key, 32));
    let a_rows = std::slice::from_raw_parts(a_rows, na);
    let b_cols = std::slice::from_raw_parts(b_cols, nb);

    let proof = PlainProof {
        m,
        n,
        k,
        noise_rank: rank,
        a: matrix_proof(a, key32, a_rows, k),
        bt: matrix_proof(b, key32, b_cols, k),
        moe: None,
    };
    use base64::{engine::general_purpose::STANDARD, Engine as _};
    let b64 = STANDARD.encode(bincode::serialize(&proof).unwrap());
    if b64.len() > cap {
        return -1;
    }
    std::ptr::copy_nonoverlapping(b64.as_ptr(), out, b64.len());
    b64.len() as isize
}
