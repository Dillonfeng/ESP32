"""
Convert 海燕收音机 channels.json → ESP32 binary format (stations.bin)

Binary layout (all little-endian):
  HEADER (24 bytes):
    magic: "ESP32RDB" (8 bytes)
    version: uint8 = 2
    num_regions: uint16
    num_stations: uint32
    region_idx_offset: uint32
    station_records_offset: uint32
    reserved: 3 bytes

  REGION INDEX (region_idx_offset):
    For each region (num_regions, 28 bytes each):
      name: 16 bytes UTF-8 null-padded
      first_station_offset: uint32  (byte offset in station records section)
      station_count: uint16
      reserved: 6 bytes

  STATION RECORDS (station_records_offset, variable per station):
    name_len: uint8
    name: name_len bytes UTF-8
    url_len: uint16
    url: url_len bytes ASCII
    region_idx: uint8
    category_len: uint8
    category: category_len bytes UTF-8
    freq_len: uint8
    freq: freq_len bytes UTF-8
    color_rgb: 3 bytes (RR, GG, BB)
"""

import json, os, struct, sys

INPUT = r"d:\ESP32-S3\海燕收音机_电台列表_20260912_200200.channels.json"
OUTPUT = r"d:\ESP32-S3\data\stations.bin"

def hex_to_rgb(h):
    h = h.lstrip('#')
    if len(h) != 6: return (200, 200, 200)
    try: return (int(h[0:2],16), int(h[2:4],16), int(h[4:6],16))
    except: return (200, 200, 200)

def pack_str(s, max_len=255):
    """UTF-8 encode, return (bytes, actual_len)"""
    b = s.encode('utf-8')[:max_len]
    return b, len(b)

def main():
    with open(INPUT, 'r', encoding='utf-8') as f:
        data = json.load(f)

    stations = data['radio']
    print(f"Input: {len(stations)} stations")

    # Step 1: Build region index
    regions = []  # list of region names
    region_map = {}  # desc → region_idx
    station_regions = []  # region_idx for each station

    for s in stations:
        desc = s.get('description', '其它')
        if desc not in region_map:
            region_map[desc] = len(regions)
            regions.append(desc)
        station_regions.append(region_map[desc])

    print(f"Regions: {len(regions)}")

    # Step 2: Sort stations by region (so same-region stations are contiguous)
    # Build list of (station_orig_idx, region_idx)
    indexed = list(enumerate(station_regions))
    indexed.sort(key=lambda x: x[1])

    # Step 3: Build region table with offsets
    # We'll first pack all station records, then compute offsets
    station_blobs = []  # list of (blob, region_idx)
    station_offsets = []  # offset in packed section for each (sorted) station

    for orig_idx, region_idx in indexed:
        s = stations[orig_idx]
        blob = b''
        # name
        name_b, name_l = pack_str(s.get('name', ''), 60)
        blob += struct.pack('B', name_l) + name_b
        # url
        url_b, url_l = pack_str(s.get('url', ''), 512)
        blob += struct.pack('H', url_l) + url_b
        # region_idx
        blob += struct.pack('B', region_idx)
        # category
        cat_b, cat_l = pack_str(s.get('category', ''), 20)
        blob += struct.pack('B', cat_l) + cat_b
        # freq
        freq_b, freq_l = pack_str(s.get('frequency', ''), 20)
        blob += struct.pack('B', freq_l) + freq_b
        # color
        r, g, b = hex_to_rgb(s.get('color', '#cccccc'))
        blob += struct.pack('BBB', r, g, b)

        station_blobs.append((blob, region_idx))

    # Now compute offsets and build region table
    STATION_RECORDS_OFFSET = 24 + 28 * len(regions)  # header + region table
    cur_offset = STATION_RECORDS_OFFSET

    region_entries = []  # (first_station_offset, station_count)
    region_counts = [0] * len(regions)
    region_first = [None] * len(regions)

    for blob, region_idx in station_blobs:
        if region_first[region_idx] is None:
            region_first[region_idx] = cur_offset
        region_counts[region_idx] += 1
        station_offsets.append(cur_offset)
        cur_offset += len(blob)

    for i in range(len(regions)):
        first_off = region_first[i] if region_first[i] is not None else STATION_RECORDS_OFFSET
        region_entries.append((first_off, region_counts[i]))

    # Step 4: Write file
    os.makedirs(os.path.dirname(OUTPUT), exist_ok=True)
    with open(OUTPUT, 'wb') as f:
        # HEADER (24 bytes)
        f.write(b'ESP32RDB')
        f.write(struct.pack('B', 2))  # version
        f.write(struct.pack('H', len(regions)))  # num_regions
        f.write(struct.pack('I', len(stations)))  # num_stations
        f.write(struct.pack('I', 24))  # region_idx_offset (always 24)
        f.write(struct.pack('I', STATION_RECORDS_OFFSET))  # station_records_offset
        f.write(b'\x00')  # 1 byte reserved → total 24

        # REGION INDEX (28 bytes each)
        for i, name in enumerate(regions):
            name_b = name.encode('utf-8')[:15].ljust(16, b'\x00')
            first_off, count = region_entries[i]
            f.write(name_b)
            f.write(struct.pack('I', first_off))
            f.write(struct.pack('H', count))
            f.write(b'\x00' * 6)

        # STATION RECORDS
        for blob, _ in station_blobs:
            f.write(blob)

    size = os.path.getsize(OUTPUT)
    print(f"Output: {OUTPUT} ({size:,} bytes)")
    print(f"  Header: 24")
    print(f"  Region idx: {28 * len(regions):,}")
    print(f"  Station records: {size - STATION_RECORDS_OFFSET:,}")
    print()

    # Quick stats
    url_lens = [len(stations[i]['url']) for i, _ in indexed]
    print(f"URL max: {max(url_lens)}, 95%: {sorted(url_lens)[-len(url_lens)//20]}")
    name_lens = [len(stations[i]['name'].encode('utf-8')) for i, _ in indexed]
    print(f"Name (utf8) max: {max(name_lens)}, 95%: {sorted(name_lens)[-len(name_lens)//20]}")

    # Show regions sorted by count
    print(f"\n=== Regions sorted by count ===")
    pairs = [(regions[i], region_counts[i]) for i in range(len(regions))]
    pairs.sort(key=lambda x: -x[1])
    for name, cnt in pairs:
        print(f"  {name}: {cnt}")

if __name__ == '__main__':
    main()
