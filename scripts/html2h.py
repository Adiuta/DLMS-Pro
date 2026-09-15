import gzip
import os

html_file = 'data/index.html'
header_file = 'include/WebUI.h'

print(f"Compressing {html_file} to {header_file}...")

with open(html_file, 'rb') as f:
    html_data = f.read()

compressed_data = gzip.compress(html_data)

with open(header_file, 'w') as f:
    f.write('#ifndef WEB_UI_H\n#define WEB_UI_H\n\n')
    f.write('#include <Arduino.h>\n\n')
    f.write(f'const size_t index_html_gz_len = {len(compressed_data)};\n')
    f.write('const uint8_t index_html_gz[] PROGMEM = {\n')
    
    # write bytes in hex
    hex_array = [f'0x{b:02x}' for b in compressed_data]
    
    # Format to 12 hex bytes per line
    for i in range(0, len(hex_array), 12):
        f.write('    ' + ', '.join(hex_array[i:i+12]) + ',\n')
        
    f.write('};\n\n')
    f.write('#endif // WEB_UI_H\n')

print(f"Done. Compressed size: {len(compressed_data)} bytes (Original: {len(html_data)} bytes)")
