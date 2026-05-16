import gzip
import os

def html_to_header(html_path, header_path, var_name):
    with open(html_path, 'rb') as f:
        content = f.read()
    
    compressed = gzip.compress(content)
    
    os.makedirs(os.path.dirname(header_path), exist_ok=True)
    
    with open(header_path, 'w') as f:
        f.write(f'#pragma once\n\n')
        f.write(f'const uint8_t {var_name}_gz[] = {{\n')
        for i, byte in enumerate(compressed):
            if i % 16 == 0:
                f.write('  ')
            f.write(f'0x{byte:02x}')
            if i < len(compressed) - 1:
                f.write(', ')
            if (i + 1) % 16 == 0:
                f.write('\n')
        f.write('\n};\n\n')
        f.write(f'const uint32_t {var_name}_gz_len = {len(compressed)};\n')
    
    print(f'Generated: {header_path}')

# Измени пути если нужно
base = os.path.dirname(os.path.abspath(__file__))

html_to_header(
    os.path.join(base, 'index.html'),
    os.path.join(base, 'generated', 'index_html.h'),
    'src_generated_index_html'
)

html_to_header(
    os.path.join(base, 'ota.html'),
    os.path.join(base, 'generated', 'ota_html.h'),
    'src_generated_ota_html'
)

print('Готово!')