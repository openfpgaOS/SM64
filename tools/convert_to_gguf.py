#!/usr/bin/env python3
"""
Convert llama2.c format (model.bin + tokenizer.bin) to GGUF format.

Usage:
    python convert_to_gguf.py model.bin tokenizer.bin output.gguf

The llama2.c format is:
- model.bin: Config header (7 int32s) followed by float32 weights
- tokenizer.bin: vocab_size (int32), max_token_length (int32), then tokens

GGUF format specification:
https://github.com/ggerganov/ggml/blob/master/docs/gguf.md
"""

import struct
import sys
import os
from pathlib import Path

# GGUF constants
GGUF_MAGIC = 0x46554747  # "GGUF" in little-endian
GGUF_VERSION = 3

# GGUF value types
GGUF_TYPE_UINT32 = 4
GGUF_TYPE_INT32 = 5
GGUF_TYPE_FLOAT32 = 6
GGUF_TYPE_STRING = 8
GGUF_TYPE_ARRAY = 9

# GGML tensor types
GGML_TYPE_F32 = 0
GGML_TYPE_F16 = 1


def write_gguf_string(f, s):
    """Write a GGUF string (length + bytes, NOT null-terminated)."""
    encoded = s.encode('utf-8')
    f.write(struct.pack('<Q', len(encoded)))
    f.write(encoded)


def write_gguf_kv_string(f, key, value):
    """Write a string key-value pair."""
    write_gguf_string(f, key)
    f.write(struct.pack('<I', GGUF_TYPE_STRING))
    write_gguf_string(f, value)


def write_gguf_kv_uint32(f, key, value):
    """Write a uint32 key-value pair."""
    write_gguf_string(f, key)
    f.write(struct.pack('<I', GGUF_TYPE_UINT32))
    f.write(struct.pack('<I', value))


def write_gguf_kv_int32(f, key, value):
    """Write an int32 key-value pair."""
    write_gguf_string(f, key)
    f.write(struct.pack('<I', GGUF_TYPE_INT32))
    f.write(struct.pack('<i', value))


def write_gguf_kv_float32(f, key, value):
    """Write a float32 key-value pair."""
    write_gguf_string(f, key)
    f.write(struct.pack('<I', GGUF_TYPE_FLOAT32))
    f.write(struct.pack('<f', value))


def write_gguf_kv_string_array(f, key, values):
    """Write a string array key-value pair."""
    write_gguf_string(f, key)
    f.write(struct.pack('<I', GGUF_TYPE_ARRAY))
    f.write(struct.pack('<I', GGUF_TYPE_STRING))
    f.write(struct.pack('<Q', len(values)))
    for v in values:
        write_gguf_string(f, v)


def read_llama2c_config(model_data):
    """Read the llama2.c config header (7 int32s)."""
    config = struct.unpack('<7i', model_data[:28])
    return {
        'dim': config[0],
        'hidden_dim': config[1],
        'n_layers': config[2],
        'n_heads': config[3],
        'n_kv_heads': config[4],
        'vocab_size': abs(config[5]),  # Negative means shared weights
        'seq_len': config[6],
        'shared_weights': config[5] > 0,  # Positive = shared in llama2.c
    }


def read_llama2c_tokenizer(tokenizer_data, vocab_size):
    """Read the llama2.c tokenizer format.

    Format: max_token_length (int32), then for each token:
            score (float32), length (int32), bytes
    """
    offset = 0
    max_token_len, = struct.unpack('<i', tokenizer_data[offset:offset+4])
    offset += 4

    print(f"  max_token_length: {max_token_len}")

    tokens = []
    scores = []

    for i in range(vocab_size):
        score, = struct.unpack('<f', tokenizer_data[offset:offset+4])
        offset += 4
        token_len, = struct.unpack('<i', tokenizer_data[offset:offset+4])
        offset += 4
        token = tokenizer_data[offset:offset+token_len].decode('utf-8', errors='replace')
        offset += token_len
        tokens.append(token)
        scores.append(score)

    return tokens, scores


def convert_to_gguf(model_path, tokenizer_path, output_path, use_fp16=False):
    """Convert llama2.c format to GGUF."""

    print(f"Reading model from {model_path}...")
    with open(model_path, 'rb') as f:
        model_data = f.read()

    print(f"Reading tokenizer from {tokenizer_path}...")
    with open(tokenizer_path, 'rb') as f:
        tokenizer_data = f.read()

    # Parse config
    config = read_llama2c_config(model_data)
    print(f"Model config: {config}")

    # Parse tokenizer (needs vocab_size from config)
    tokens, scores = read_llama2c_tokenizer(tokenizer_data, config['vocab_size'])
    print(f"Tokenizer: {len(tokens)} tokens")

    # Calculate weight sizes
    dim = config['dim']
    hidden_dim = config['hidden_dim']
    n_layers = config['n_layers']
    n_heads = config['n_heads']
    n_kv_heads = config['n_kv_heads']
    vocab_size = config['vocab_size']
    seq_len = config['seq_len']
    head_size = dim // n_heads
    kv_dim = head_size * n_kv_heads

    # =====================================================
    # Read weights from model.bin in the correct order
    # model.bin layout (all weights are float32):
    #   config (28 bytes)
    #   token_embedding_table[vocab_size * dim]
    #   rms_att_weight[n_layers * dim]
    #   wq[n_layers * dim * dim]
    #   wk[n_layers * kv_dim * dim]
    #   wv[n_layers * kv_dim * dim]
    #   wo[n_layers * dim * dim]
    #   rms_ffn_weight[n_layers * dim]
    #   w1[n_layers * hidden_dim * dim]
    #   w2[n_layers * dim * hidden_dim]
    #   w3[n_layers * hidden_dim * dim]
    #   rms_final_weight[dim]
    #   freq_cis_real[seq_len * head_size / 2]  (skipped)
    #   freq_cis_imag[seq_len * head_size / 2]  (skipped)
    #   wcls[vocab_size * dim]  (if not shared)
    # =====================================================

    def read_weights(offset, count):
        """Read count float32 values from model_data at offset."""
        end = offset + count * 4
        data = model_data[offset:end]
        return struct.unpack(f'<{count}f', data), end

    offset = 28  # Skip config header

    # Read all weight tensors from model.bin
    print("Reading weights from model.bin...")

    token_embd, offset = read_weights(offset, vocab_size * dim)
    print(f"  token_embd: {vocab_size} x {dim}")

    rms_att_all, offset = read_weights(offset, n_layers * dim)
    print(f"  rms_att: {n_layers} x {dim}")

    wq_all, offset = read_weights(offset, n_layers * dim * dim)
    print(f"  wq: {n_layers} x {dim} x {dim}")

    wk_all, offset = read_weights(offset, n_layers * kv_dim * dim)
    print(f"  wk: {n_layers} x {kv_dim} x {dim}")

    wv_all, offset = read_weights(offset, n_layers * kv_dim * dim)
    print(f"  wv: {n_layers} x {kv_dim} x {dim}")

    wo_all, offset = read_weights(offset, n_layers * dim * dim)
    print(f"  wo: {n_layers} x {dim} x {dim}")

    rms_ffn_all, offset = read_weights(offset, n_layers * dim)
    print(f"  rms_ffn: {n_layers} x {dim}")

    w1_all, offset = read_weights(offset, n_layers * hidden_dim * dim)
    print(f"  w1: {n_layers} x {hidden_dim} x {dim}")

    w2_all, offset = read_weights(offset, n_layers * dim * hidden_dim)
    print(f"  w2: {n_layers} x {dim} x {hidden_dim}")

    w3_all, offset = read_weights(offset, n_layers * hidden_dim * dim)
    print(f"  w3: {n_layers} x {hidden_dim} x {dim}")

    rms_final, offset = read_weights(offset, dim)
    print(f"  rms_final: {dim}")

    # Skip freq_cis (RoPE precomputed, not used in GGUF)
    freq_cis_size = seq_len * head_size // 2
    offset += freq_cis_size * 4  # freq_cis_real
    offset += freq_cis_size * 4  # freq_cis_imag

    # Check for separate output weights
    remaining_bytes = len(model_data) - offset
    wcls_size = vocab_size * dim * 4
    if remaining_bytes >= wcls_size and not config['shared_weights']:
        wcls, offset = read_weights(offset, vocab_size * dim)
        has_wcls = True
        print(f"  wcls: {vocab_size} x {dim}")
    else:
        wcls = None
        has_wcls = False
        print("  wcls: shared with token_embd")

    # =====================================================
    # Build GGUF tensor list with proper per-layer extraction
    # =====================================================

    # Helper to extract per-layer slice from flattened array
    def get_layer_weights(all_weights, layer, layer_size):
        start = layer * layer_size
        return all_weights[start:start + layer_size]

    # Define tensors for GGUF (metadata order)
    tensors = []
    tensor_data = {}

    # Token embeddings
    tensors.append(('token_embd.weight', [vocab_size, dim]))
    tensor_data['token_embd.weight'] = token_embd

    # Per-layer weights
    for l in range(n_layers):
        # RMS norm (attention)
        name = f'blk.{l}.attn_norm.weight'
        tensors.append((name, [dim]))
        tensor_data[name] = get_layer_weights(rms_att_all, l, dim)

        # Attention Q, K, V, O
        name = f'blk.{l}.attn_q.weight'
        tensors.append((name, [dim, dim]))
        tensor_data[name] = get_layer_weights(wq_all, l, dim * dim)

        name = f'blk.{l}.attn_k.weight'
        tensors.append((name, [kv_dim, dim]))
        tensor_data[name] = get_layer_weights(wk_all, l, kv_dim * dim)

        name = f'blk.{l}.attn_v.weight'
        tensors.append((name, [kv_dim, dim]))
        tensor_data[name] = get_layer_weights(wv_all, l, kv_dim * dim)

        name = f'blk.{l}.attn_output.weight'
        tensors.append((name, [dim, dim]))
        tensor_data[name] = get_layer_weights(wo_all, l, dim * dim)

        # RMS norm (FFN)
        name = f'blk.{l}.ffn_norm.weight'
        tensors.append((name, [dim]))
        tensor_data[name] = get_layer_weights(rms_ffn_all, l, dim)

        # FFN gate, down, up
        name = f'blk.{l}.ffn_gate.weight'
        tensors.append((name, [hidden_dim, dim]))
        tensor_data[name] = get_layer_weights(w1_all, l, hidden_dim * dim)

        name = f'blk.{l}.ffn_down.weight'
        tensors.append((name, [dim, hidden_dim]))
        tensor_data[name] = get_layer_weights(w2_all, l, dim * hidden_dim)

        name = f'blk.{l}.ffn_up.weight'
        tensors.append((name, [hidden_dim, dim]))
        tensor_data[name] = get_layer_weights(w3_all, l, hidden_dim * dim)

    # Final RMS norm
    tensors.append(('output_norm.weight', [dim]))
    tensor_data['output_norm.weight'] = rms_final

    # Output projection (if not shared)
    if has_wcls:
        tensors.append(('output.weight', [vocab_size, dim]))
        tensor_data['output.weight'] = wcls

    # Build metadata KV pairs
    metadata = [
        ('general.architecture', 'string', 'llama'),
        ('general.name', 'string', 'llama2c-converted'),
        ('llama.context_length', 'uint32', seq_len),
        ('llama.embedding_length', 'uint32', dim),
        ('llama.block_count', 'uint32', n_layers),
        ('llama.feed_forward_length', 'uint32', hidden_dim),
        ('llama.attention.head_count', 'uint32', n_heads),
        ('llama.attention.head_count_kv', 'uint32', n_kv_heads),
        ('llama.rope.dimension_count', 'uint32', head_size),
        ('tokenizer.ggml.model', 'string', 'llama'),
        ('tokenizer.ggml.tokens', 'string_array', tokens),
        ('tokenizer.ggml.scores', 'float_array', scores),
        ('tokenizer.ggml.bos_token_id', 'uint32', 1),
        ('tokenizer.ggml.eos_token_id', 'uint32', 2),
    ]

    print(f"Writing GGUF to {output_path}...")

    with open(output_path, 'wb') as f:
        # Write header
        f.write(struct.pack('<I', GGUF_MAGIC))
        f.write(struct.pack('<I', GGUF_VERSION))
        f.write(struct.pack('<Q', len(tensors)))  # n_tensors
        f.write(struct.pack('<Q', len(metadata)))  # n_kv

        # Write metadata
        for key, vtype, value in metadata:
            if vtype == 'string':
                write_gguf_kv_string(f, key, value)
            elif vtype == 'uint32':
                write_gguf_kv_uint32(f, key, value)
            elif vtype == 'int32':
                write_gguf_kv_int32(f, key, value)
            elif vtype == 'float32':
                write_gguf_kv_float32(f, key, value)
            elif vtype == 'string_array':
                write_gguf_kv_string_array(f, key, value)
            elif vtype == 'float_array':
                # Write float array
                write_gguf_string(f, key)
                f.write(struct.pack('<I', GGUF_TYPE_ARRAY))
                f.write(struct.pack('<I', GGUF_TYPE_FLOAT32))
                f.write(struct.pack('<Q', len(value)))
                for v in value:
                    f.write(struct.pack('<f', v))

        # Calculate offset for tensor data (aligned to 32 bytes)
        tensor_info_start = f.tell()

        # First pass: calculate tensor info size
        tensor_type = GGML_TYPE_F16 if use_fp16 else GGML_TYPE_F32
        bytes_per_element = 2 if use_fp16 else 4

        # Calculate tensor info section size
        tensor_info_size = 0
        for name, shape in tensors:
            tensor_info_size += 8 + len(name.encode('utf-8'))  # name string
            tensor_info_size += 4  # n_dims
            tensor_info_size += 8 * len(shape)  # dims
            tensor_info_size += 4  # type
            tensor_info_size += 8  # offset

        # Data section starts after tensor infos, aligned to 32 bytes
        data_start_offset = tensor_info_start + tensor_info_size
        data_start_offset = (data_start_offset + 31) & ~31

        # Write tensor infos
        current_offset = 0
        for name, shape in tensors:
            write_gguf_string(f, name)
            f.write(struct.pack('<I', len(shape)))  # n_dims
            for dim_size in shape:
                f.write(struct.pack('<Q', dim_size))
            f.write(struct.pack('<I', tensor_type))
            f.write(struct.pack('<Q', current_offset))

            # Calculate size and advance offset
            n_elements = 1
            for d in shape:
                n_elements *= d
            current_offset += n_elements * bytes_per_element

        # Pad to alignment
        current_pos = f.tell()
        padding_needed = data_start_offset - current_pos
        if padding_needed > 0:
            f.write(b'\x00' * padding_needed)

        # Write tensor data
        print("Writing tensor data...")
        for name, shape in tensors:
            weights = tensor_data[name]
            n_elements = 1
            for d in shape:
                n_elements *= d

            if use_fp16:
                # Convert to FP16
                import numpy as np
                weights_np = np.array(weights, dtype=np.float32)
                weights_fp16 = weights_np.astype(np.float16)
                f.write(weights_fp16.tobytes())
            else:
                # Write as F32
                f.write(struct.pack(f'<{n_elements}f', *weights))

            print(f"  {name}: {shape} ({n_elements} elements)")

    output_size = os.path.getsize(output_path)
    print(f"Done! Output: {output_path} ({output_size / 1024 / 1024:.2f} MB)")


def main():
    if len(sys.argv) < 4:
        print("Usage: python convert_to_gguf.py model.bin tokenizer.bin output.gguf [--fp16]")
        print()
        print("Options:")
        print("  --fp16    Convert weights to FP16 (requires numpy)")
        sys.exit(1)

    model_path = sys.argv[1]
    tokenizer_path = sys.argv[2]
    output_path = sys.argv[3]
    use_fp16 = '--fp16' in sys.argv

    if not os.path.exists(model_path):
        print(f"Error: Model file not found: {model_path}")
        sys.exit(1)

    if not os.path.exists(tokenizer_path):
        print(f"Error: Tokenizer file not found: {tokenizer_path}")
        sys.exit(1)

    convert_to_gguf(model_path, tokenizer_path, output_path, use_fp16)


if __name__ == '__main__':
    main()
