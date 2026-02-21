#!/usr/bin/env python3
"""
Convert Q8_0 GGUF model to Q16.16 fixed-point GGUF model.

Q8_0 format: 34 bytes per 32 elements (2 byte FP16 scale + 32 int8 values)
Q16.16 format: 4 bytes per element (32-bit signed fixed-point)

This is the native format for the DMA dot product accelerator.
No runtime conversion needed - weights can be used directly.

For a 14MB Q8 model (~13M elements):
  - Q8: 14MB
  - Q16.16: 52MB (fits in 64MB SDRAM with ~12MB for runtime)

Usage:
    python tools/convert_q8_to_q16.py input.gguf output.gguf
"""

import struct
import sys
import numpy as np
from pathlib import Path

# GGUF constants
GGUF_MAGIC = 0x46554747  # "GGUF"
GGML_TYPE_F32 = 0
GGML_TYPE_F16 = 1
GGML_TYPE_Q8_0 = 8
GGML_TYPE_I32 = 18  # Use I32 type for Q16.16 fixed-point

# GGUF value types
GGUF_TYPE_UINT8 = 0
GGUF_TYPE_INT8 = 1
GGUF_TYPE_UINT16 = 2
GGUF_TYPE_INT16 = 3
GGUF_TYPE_UINT32 = 4
GGUF_TYPE_INT32 = 5
GGUF_TYPE_FLOAT32 = 6
GGUF_TYPE_BOOL = 7
GGUF_TYPE_STRING = 8
GGUF_TYPE_ARRAY = 9
GGUF_TYPE_UINT64 = 10
GGUF_TYPE_INT64 = 11
GGUF_TYPE_FLOAT64 = 12


def fp16_to_float(h):
    """Convert FP16 (uint16) to float32."""
    return np.frombuffer(np.array([h], dtype=np.uint16).tobytes(), dtype=np.float16)[0].astype(np.float32)


def dequantize_q8_0(data, n_elements):
    """Dequantize Q8_0 data to float32 array."""
    n_blocks = (n_elements + 31) // 32
    result = np.zeros(n_elements, dtype=np.float32)

    for b in range(n_blocks):
        block_start = b * 34
        # Read scale (FP16)
        scale_fp16 = struct.unpack('<H', data[block_start:block_start+2])[0]
        scale = fp16_to_float(scale_fp16)

        # Read 32 int8 values
        values = np.frombuffer(data[block_start+2:block_start+34], dtype=np.int8)

        # Dequantize
        start_idx = b * 32
        end_idx = min(start_idx + 32, n_elements)
        result[start_idx:end_idx] = scale * values[:end_idx - start_idx]

    return result


def float_to_q16_16(float_array):
    """Convert float32 array to Q16.16 fixed-point (int32) array."""
    # Q16.16: multiply by 2^16 = 65536
    # Clamp to int32 range to avoid overflow
    scaled = float_array * 65536.0
    scaled = np.clip(scaled, -2147483648, 2147483647)
    return scaled.astype(np.int32)


def read_gguf_string(f):
    """Read GGUF string (length + bytes)."""
    length = struct.unpack('<Q', f.read(8))[0]
    return f.read(length)


def write_gguf_string(f, s):
    """Write GGUF string (length + bytes)."""
    if isinstance(s, str):
        s = s.encode('utf-8')
    f.write(struct.pack('<Q', len(s)))
    f.write(s)


def read_gguf_value(f, vtype):
    """Read a GGUF value of given type."""
    if vtype == GGUF_TYPE_UINT8:
        return struct.unpack('<B', f.read(1))[0]
    elif vtype == GGUF_TYPE_INT8:
        return struct.unpack('<b', f.read(1))[0]
    elif vtype == GGUF_TYPE_UINT16:
        return struct.unpack('<H', f.read(2))[0]
    elif vtype == GGUF_TYPE_INT16:
        return struct.unpack('<h', f.read(2))[0]
    elif vtype == GGUF_TYPE_UINT32:
        return struct.unpack('<I', f.read(4))[0]
    elif vtype == GGUF_TYPE_INT32:
        return struct.unpack('<i', f.read(4))[0]
    elif vtype == GGUF_TYPE_FLOAT32:
        return struct.unpack('<f', f.read(4))[0]
    elif vtype == GGUF_TYPE_BOOL:
        return struct.unpack('<B', f.read(1))[0] != 0
    elif vtype == GGUF_TYPE_STRING:
        return read_gguf_string(f)
    elif vtype == GGUF_TYPE_ARRAY:
        arr_type = struct.unpack('<I', f.read(4))[0]
        count = struct.unpack('<Q', f.read(8))[0]
        return [read_gguf_value(f, arr_type) for _ in range(count)]
    elif vtype == GGUF_TYPE_UINT64:
        return struct.unpack('<Q', f.read(8))[0]
    elif vtype == GGUF_TYPE_INT64:
        return struct.unpack('<q', f.read(8))[0]
    elif vtype == GGUF_TYPE_FLOAT64:
        return struct.unpack('<d', f.read(8))[0]
    else:
        raise ValueError(f"Unknown GGUF value type: {vtype}")


def write_gguf_value(f, vtype, value):
    """Write a GGUF value of given type."""
    if vtype == GGUF_TYPE_UINT8:
        f.write(struct.pack('<B', value))
    elif vtype == GGUF_TYPE_INT8:
        f.write(struct.pack('<b', value))
    elif vtype == GGUF_TYPE_UINT16:
        f.write(struct.pack('<H', value))
    elif vtype == GGUF_TYPE_INT16:
        f.write(struct.pack('<h', value))
    elif vtype == GGUF_TYPE_UINT32:
        f.write(struct.pack('<I', value))
    elif vtype == GGUF_TYPE_INT32:
        f.write(struct.pack('<i', value))
    elif vtype == GGUF_TYPE_FLOAT32:
        f.write(struct.pack('<f', value))
    elif vtype == GGUF_TYPE_BOOL:
        f.write(struct.pack('<B', 1 if value else 0))
    elif vtype == GGUF_TYPE_STRING:
        write_gguf_string(f, value)
    elif vtype == GGUF_TYPE_ARRAY:
        # Arrays need special handling - write type and count first
        arr_type, items = value
        f.write(struct.pack('<I', arr_type))
        f.write(struct.pack('<Q', len(items)))
        for item in items:
            write_gguf_value(f, arr_type, item)
    elif vtype == GGUF_TYPE_UINT64:
        f.write(struct.pack('<Q', value))
    elif vtype == GGUF_TYPE_INT64:
        f.write(struct.pack('<q', value))
    elif vtype == GGUF_TYPE_FLOAT64:
        f.write(struct.pack('<d', value))


def convert_q8_to_q16(input_path, output_path):
    """Convert Q8_0 GGUF to Q16.16 fixed-point GGUF."""
    print(f"Converting {input_path} -> {output_path}")

    with open(input_path, 'rb') as f:
        # Read header
        magic = struct.unpack('<I', f.read(4))[0]
        if magic != GGUF_MAGIC:
            raise ValueError(f"Invalid GGUF magic: 0x{magic:08X}")

        version = struct.unpack('<I', f.read(4))[0]
        n_tensors = struct.unpack('<Q', f.read(8))[0]
        n_kv = struct.unpack('<Q', f.read(8))[0]

        print(f"GGUF v{version}: {n_tensors} tensors, {n_kv} metadata entries")

        # Read metadata
        metadata = []
        for _ in range(n_kv):
            key = read_gguf_string(f)
            vtype = struct.unpack('<I', f.read(4))[0]
            value = read_gguf_value(f, vtype)
            metadata.append((key, vtype, value))

        # Read tensor infos
        tensor_infos = []
        for _ in range(n_tensors):
            name = read_gguf_string(f)
            n_dims = struct.unpack('<I', f.read(4))[0]
            dims = [struct.unpack('<Q', f.read(8))[0] for _ in range(n_dims)]
            dtype = struct.unpack('<I', f.read(4))[0]
            offset = struct.unpack('<Q', f.read(8))[0]
            tensor_infos.append({
                'name': name,
                'n_dims': n_dims,
                'dims': dims,
                'dtype': dtype,
                'offset': offset
            })

        # Calculate data section alignment
        alignment = 32 if version >= 3 else 4
        current_pos = f.tell()
        data_start = ((current_pos + alignment - 1) // alignment) * alignment
        f.seek(data_start)

        # Read tensor data
        tensor_data = {}
        for info in tensor_infos:
            n_elements = 1
            for d in info['dims']:
                n_elements *= d

            if info['dtype'] == GGML_TYPE_Q8_0:
                n_blocks = (n_elements + 31) // 32
                size = n_blocks * 34
            elif info['dtype'] == GGML_TYPE_F16:
                size = n_elements * 2
            elif info['dtype'] == GGML_TYPE_F32:
                size = n_elements * 4
            elif info['dtype'] == GGML_TYPE_I32:
                size = n_elements * 4
            else:
                raise ValueError(f"Unknown tensor type: {info['dtype']}")

            f.seek(data_start + info['offset'])
            tensor_data[info['name']] = f.read(size)

    # Convert Q8 tensors to Q16.16
    converted_data = {}
    q8_count = 0
    total_elements = 0
    for info in tensor_infos:
        name = info['name']
        n_elements = 1
        for d in info['dims']:
            n_elements *= d

        if info['dtype'] == GGML_TYPE_Q8_0:
            print(f"  Converting {name.decode()}: Q8_0 -> Q16.16 ({n_elements:,} elements)")
            float_data = dequantize_q8_0(tensor_data[name], n_elements)
            q16_data = float_to_q16_16(float_data)
            converted_data[name] = q16_data.tobytes()
            info['dtype'] = GGML_TYPE_I32  # Update type to I32
            q8_count += 1
            total_elements += n_elements
        elif info['dtype'] == GGML_TYPE_F16:
            # Convert FP16 to Q16.16 as well
            print(f"  Converting {name.decode()}: F16 -> Q16.16 ({n_elements:,} elements)")
            fp16_array = np.frombuffer(tensor_data[name], dtype=np.float16)
            float_data = fp16_array.astype(np.float32)
            q16_data = float_to_q16_16(float_data)
            converted_data[name] = q16_data.tobytes()
            info['dtype'] = GGML_TYPE_I32
            total_elements += n_elements
        elif info['dtype'] == GGML_TYPE_F32:
            # Convert F32 to Q16.16 as well
            print(f"  Converting {name.decode()}: F32 -> Q16.16 ({n_elements:,} elements)")
            float_data = np.frombuffer(tensor_data[name], dtype=np.float32)
            q16_data = float_to_q16_16(float_data)
            converted_data[name] = q16_data.tobytes()
            info['dtype'] = GGML_TYPE_I32
            total_elements += n_elements
        else:
            converted_data[name] = tensor_data[name]

    print(f"Converted {q8_count} Q8 tensors ({total_elements:,} elements total)")

    # Write output GGUF
    with open(output_path, 'wb') as f:
        # Header
        f.write(struct.pack('<I', GGUF_MAGIC))
        f.write(struct.pack('<I', version))
        f.write(struct.pack('<Q', n_tensors))
        f.write(struct.pack('<Q', n_kv))

        # Metadata
        for key, vtype, value in metadata:
            write_gguf_string(f, key)
            f.write(struct.pack('<I', vtype))
            if vtype == GGUF_TYPE_ARRAY:
                # Need to determine array type from first element
                if isinstance(value, list) and len(value) > 0:
                    if isinstance(value[0], bytes):
                        arr_type = GGUF_TYPE_STRING
                    elif isinstance(value[0], int):
                        arr_type = GGUF_TYPE_UINT32
                    elif isinstance(value[0], float):
                        arr_type = GGUF_TYPE_FLOAT32
                    else:
                        arr_type = GGUF_TYPE_UINT32
                    write_gguf_value(f, vtype, (arr_type, value))
                else:
                    write_gguf_value(f, vtype, (GGUF_TYPE_UINT32, []))
            else:
                write_gguf_value(f, vtype, value)

        # Calculate new tensor offsets
        current_offset = 0
        new_offsets = {}
        for info in tensor_infos:
            name = info['name']
            new_offsets[name] = current_offset
            current_offset += len(converted_data[name])
            # Align to 32 bytes
            current_offset = ((current_offset + 31) // 32) * 32

        # Tensor infos
        for info in tensor_infos:
            name = info['name']
            write_gguf_string(f, name)
            f.write(struct.pack('<I', info['n_dims']))
            for d in info['dims']:
                f.write(struct.pack('<Q', d))
            f.write(struct.pack('<I', info['dtype']))
            f.write(struct.pack('<Q', new_offsets[name]))

        # Pad to alignment
        current_pos = f.tell()
        pad_to = ((current_pos + alignment - 1) // alignment) * alignment
        f.write(b'\x00' * (pad_to - current_pos))

        # Tensor data
        for info in tensor_infos:
            name = info['name']
            data = converted_data[name]
            f.write(data)
            # Pad to 32 bytes
            pad_len = (32 - (len(data) % 32)) % 32
            if pad_len:
                f.write(b'\x00' * pad_len)

    in_size = Path(input_path).stat().st_size
    out_size = Path(output_path).stat().st_size
    print(f"Done! {in_size/1024/1024:.1f}MB -> {out_size/1024/1024:.1f}MB")
    print(f"Output format: Q16.16 fixed-point (GGML_TYPE_I32)")
    print(f"Ready for direct use by DMA accelerator - no runtime conversion needed")


if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} input.gguf output.gguf")
        sys.exit(1)

    convert_q8_to_q16(sys.argv[1], sys.argv[2])
