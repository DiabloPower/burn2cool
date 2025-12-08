#!/usr/bin/env python3
import sys
if len(sys.argv) != 2:
    sys.exit(1)
data = open(sys.argv[1], 'rb').read()
var = sys.argv[1].replace('/', '_').replace('.', '_').replace('-', '_')
arr_name = var + '_data'
len_name = var + '_data_len'
print('unsigned char ' + arr_name + '[] = {')
for i in range(0, len(data), 12):
    chunk = data[i:i+12]
    print(','.join('0x%02x' % b for b in chunk) + (',' if i+12 < len(data) else ''))
print('};')
print('unsigned int ' + len_name + ' = ' + str(len(data)) + ';')