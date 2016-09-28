#include "libhzr.h"

#include <cstdint>

#include "hzr_crc32.h"
#include "hzr_internal.h"

namespace hzr {

namespace {

// The maximum size of the tree representation (there are two additional bits
// per leaf node, representing the branches in the tree).
#define kMaxTreeDataSize (((2 + kSymbolSize) * kNumSymbols + 7) / 8)

class WriteStream {
 public:
  // Initialize a bitstream.
  explicit WriteStream(void* buf)
      : m_base_ptr(reinterpret_cast<uint8_t*>(buf)),
        m_byte_ptr(reinterpret_cast<uint8_t*>(buf)),
        m_bit_pos(0) {}

  // Reset the write position (rewind).
  void Reset() {
    m_byte_ptr = m_base_ptr;
    m_bit_pos = 0;
  }

  // Write bits to a bitstream.
  void WriteBits(uint32_t x, int bits) {
    // Get current stream state.
    uint8_t* buf = m_byte_ptr;
    int bit = m_bit_pos;

    // Append bits.
    // TODO(m): Optimize this!
    while (bits--) {
      *buf = (*buf & (0xff ^ (1 << bit))) | ((x & 1) << bit);
      x >>= 1;
      bit = (bit + 1) & 7;
      if (!bit) {
        ++buf;
      }
    }

    // Store new stream state.
    m_byte_ptr = buf;
    m_bit_pos = bit;
  }

  // Align the stream to a byte boundary (do nothing if already aligned).
  void AlignToByte() {
    if (m_bit_pos) {
      m_bit_pos = 0;
      ++m_byte_ptr;
    }
  }

  // Advance N bytes.
  void AdvanceBytes(int N) { m_byte_ptr += N; }

  int Size() const {
    int total_bytes = static_cast<int>(m_byte_ptr - m_base_ptr);
    if (m_bit_pos > 0) {
      ++total_bytes;
    }
    return total_bytes;
  }

  uint8_t* byte_ptr() { return m_byte_ptr; }

 private:
  uint8_t* m_base_ptr;
  uint8_t* m_byte_ptr;
  int m_bit_pos;
};

// Used by the encoder for building the optimal Huffman tree.
struct SymbolInfo {
  Symbol symbol;
  int count;
  uint32_t code;
  int bits;
};

struct EncodeNode {
  EncodeNode *child_a, *child_b;
  int count;
  int symbol;
};

// Calculate histogram for a block of data.
void Histogram(const uint8_t* in, SymbolInfo* symbols, size_t in_size) {
  // Clear/init histogram.
  for (int k = 0; k < kNumSymbols; ++k) {
    symbols[k].symbol = static_cast<Symbol>(k);
    symbols[k].count = 0;
    symbols[k].code = 0;
    symbols[k].bits = 0;
  }

  // Build the histogram for this block.
  for (int k = 0; k < in_size;) {
    Symbol symbol = static_cast<Symbol>(in[k]);

    // Possible RLE?
    if (symbol == 0) {
      int zeros;
      for (zeros = 1; zeros < 16662 && (k + zeros) < in_size; ++zeros) {
        if (in[k + zeros] != 0) {
          break;
        }
      }
      if (zeros == 1) {
        symbols[0].count++;
      } else if (zeros == 2) {
        symbols[kSymTwoZeros].count++;
      } else if (zeros <= 6) {
        symbols[kSymUpTo6Zeros].count++;
      } else if (zeros <= 22) {
        symbols[kSymUpTo22Zeros].count++;
      } else if (zeros <= 278) {
        symbols[kSymUpTo278Zeros].count++;
      } else {
        symbols[kSymUpTo16662Zeros].count++;
      }
      k += zeros;
    } else {
      symbols[symbol].count++;
      k++;
    }
  }
}

// Store a Huffman tree in the output stream and in a look-up-table (a symbol
// array).
void StoreTree(EncodeNode* node,
               SymbolInfo* symbols,
               WriteStream* stream,
               uint32_t code,
               int bits) {
  // Is this a leaf node?
  if (node->symbol >= 0) {
    // Append symbol to tree description.
    stream->WriteBits(1, 1);
    stream->WriteBits(static_cast<uint32_t>(node->symbol), kSymbolSize);

    // Find symbol index.
    int sym_idx;
    for (sym_idx = 0; sym_idx < kNumSymbols; ++sym_idx) {
      if (symbols[sym_idx].symbol == static_cast<Symbol>(node->symbol)) {
        break;
      }
    }

    // Store code info in symbol array.
    symbols[sym_idx].code = code;
    symbols[sym_idx].bits = bits;
    return;
  }

  // This was not a leaf node.
  stream->WriteBits(0, 1);

  // Branch A.
  StoreTree(node->child_a, symbols, stream, code, bits + 1);

  // Branch B.
  StoreTree(node->child_b, symbols, stream, code + (1 << bits), bits + 1);
}

// Generate a Huffman tree.
void MakeTree(SymbolInfo* sym, WriteStream* stream) {
  // Initialize all leaf nodes.
  EncodeNode nodes[kMaxTreeNodes];
  int num_symbols = 0;
  for (int k = 0; k < kNumSymbols; ++k) {
    if (sym[k].count > 0) {
      nodes[num_symbols].symbol = static_cast<int>(sym[k].symbol);
      nodes[num_symbols].count = sym[k].count;
      nodes[num_symbols].child_a = nullptr;
      nodes[num_symbols].child_b = nullptr;
      ++num_symbols;
    }
  }

  // Special case: No symbols at all - don't store anything in the output
  // stream.
  if (num_symbols == 0) {
    return;
  }

  // Build tree by joining the lightest nodes until there is only one node left
  // (the root node).
  EncodeNode* root = nullptr;
  int nodes_left = num_symbols;
  int next_idx = num_symbols;
  while (nodes_left > 1) {
    // Find the two lightest nodes.
    EncodeNode* node_1 = nullptr;
    EncodeNode* node_2 = nullptr;
    for (int k = 0; k < next_idx; ++k) {
      if (nodes[k].count > 0) {
        if (!node_1 || (nodes[k].count <= node_1->count)) {
          node_2 = node_1;
          node_1 = &nodes[k];
        } else if (!node_2 || (nodes[k].count <= node_2->count)) {
          node_2 = &nodes[k];
        }
      }
    }

    // Join the two nodes into a new parent node.
    root = &nodes[next_idx];
    root->child_a = node_1;
    root->child_b = node_2;
    root->count = node_1->count + node_2->count;
    root->symbol = -1;
    node_1->count = 0;
    node_2->count = 0;
    ++next_idx;
    --nodes_left;
  }

  // Store the tree in the output stream, and in the sym[] array (the latter is
  // used as a look-up-table for faster encoding).
  if (root) {
    StoreTree(root, sym, stream, 0, 0);
  } else {
    // Special case: only one symbol => no binary tree.
    root = &nodes[0];
    StoreTree(root, sym, stream, 0, 1);
  }
}

}  // namespace

}  // namespace hzr

extern "C" size_t hzr_max_compressed_size(size_t uncompressed_size) {
  // TODO: Implement me!
  return uncompressed_size * 2 + kMaxTreeDataSize;
}

extern "C" hzr_status_t hzr_encode(const void* in,
                                   size_t in_size,
                                   void* out,
                                   size_t out_size,
                                   size_t* encoded_size) {
  // Check input arguments.
  if (!in || !out || !encoded_size) {
    return HZR_FAIL;
  }

  const uint8_t* in_data = reinterpret_cast<const uint8_t*>(in);

  // Initialize the output stream.
  // TODO(m): Keep track of the output size!
  (void)out_size;
  hzr::WriteStream stream(out);

  // Make room for the header.
  stream.AdvanceBytes(HZR_HEADER_SIZE);

  // Calculate the histogram for input data.
  hzr::SymbolInfo symbols[kNumSymbols];
  hzr::Histogram(in_data, symbols, in_size);

  // Build the Huffman tree, and write it to the output stream.
  hzr::MakeTree(symbols, &stream);

  // Sort symbols - first symbol first (bubble sort).
  // TODO(m): Quick-sort.
  bool swaps;
  do {
    swaps = false;
    for (int k = 0; k < kNumSymbols - 1; ++k) {
      if (symbols[k].symbol > symbols[k + 1].symbol) {
        hzr::SymbolInfo tmp = symbols[k];
        symbols[k] = symbols[k + 1];
        symbols[k + 1] = tmp;
        swaps = true;
      }
    }
  } while (swaps);

  // Encode the input stream.
  for (int k = 0; k < in_size;) {
    uint8_t symbol = in_data[k];

    // Possible RLE?
    if (symbol == 0) {
      int zeros;
      for (zeros = 1; zeros < 16662 && (k + zeros) < in_size; ++zeros) {
        if (in_data[k + zeros] != 0) {
          break;
        }
      }
      if (zeros == 1) {
        stream.WriteBits(symbols[0].code, symbols[0].bits);
      } else if (zeros == 2) {
        stream.WriteBits(symbols[kSymTwoZeros].code,
                         symbols[kSymTwoZeros].bits);
      } else if (zeros <= 6) {
        uint32_t count = static_cast<uint32_t>(zeros - 3);
        stream.WriteBits(symbols[kSymUpTo6Zeros].code,
                         symbols[kSymUpTo6Zeros].bits);
        stream.WriteBits(count, 2);
      } else if (zeros <= 22) {
        uint32_t count = static_cast<uint32_t>(zeros - 7);
        stream.WriteBits(symbols[kSymUpTo22Zeros].code,
                         symbols[kSymUpTo22Zeros].bits);
        stream.WriteBits(count, 4);
      } else if (zeros <= 278) {
        uint32_t count = static_cast<uint32_t>(zeros - 23);
        stream.WriteBits(symbols[kSymUpTo278Zeros].code,
                         symbols[kSymUpTo278Zeros].bits);
        stream.WriteBits(count, 8);
      } else {
        uint32_t count = static_cast<uint32_t>(zeros - 279);
        stream.WriteBits(symbols[kSymUpTo16662Zeros].code,
                         symbols[kSymUpTo16662Zeros].bits);
        stream.WriteBits(count, 14);
      }
      k += zeros;
    } else {
      stream.WriteBits(symbols[symbol].code, symbols[symbol].bits);
      k++;
    }
  }

  // Calculate size of output data.
  *encoded_size = stream.Size();

  // Calculate the CRC for the compressed buffer.
  uint32_t crc32 = _hzr_crc32(out, *encoded_size);

  // Update the header.
  stream.Reset();
  stream.WriteBits(static_cast<uint32_t>(in_size), 32);
  stream.WriteBits(crc32, 32);

  return HZR_OK;
}
