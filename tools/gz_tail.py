import struct, zlib, sys
d = open(sys.argv[1] if len(sys.argv) > 1 else '/tmp/p.gz', 'rb').read()
print('total size:', len(d))
# find last non-zero byte
i = len(d) - 1
while i >= 0 and d[i] == 0:
    i -= 1
print('last nonzero at:', i, ' padding zeros:', len(d) - 1 - i)
print('candidate trailer 8B before that+1:', d[i - 7:i + 1].hex())
crc, isize = struct.unpack('<II', d[i - 7:i + 1])
print('crc=%08x isize=%d' % (crc, isize))
do = zlib.decompressobj(16 + zlib.MAX_WBITS)
out = do.decompress(d)
print('actual uncompressed:', len(out))
