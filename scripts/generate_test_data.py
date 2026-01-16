#!/usr/bin/env python3
"""
生成不同大小的测试数据 JSON 文件
"""

import json
import os
import random

def generate_segment(index):
    """生成一个 segment 对象"""
    return {
        "start": index * 0.5,
        "duration": 0.5,
        "loudness": -10.0 + random.uniform(-5, 5),
        "pitches": [random.uniform(0, 1) for _ in range(12)],
        "timbre": [random.uniform(-100, 100) for _ in range(12)],
        "confidence": random.uniform(0.5, 1.0)
    }

def generate_test_data(num_segments):
    """生成包含指定数量 segments 的测试数据"""

    # 基础数据
    data = {
        "metadata": {
            "version": "1.0",
            "description": f"ShmProxy Benchmark Test Data ({num_segments} segments)",
            "num_segments": num_segments
        },
        "basic_types": {
            "string_field": "Hello World",
            "int_field": 12345,
            "float_field": 3.14159265358979,
            "bool_true": True,
            "bool_false": False,
            "null_field": None,
            "empty_string": "",
            "chinese_string": "中文测试字符串",
            "emoji_string": "🎵🎶🎸",
            "long_string": "Lorem ipsum dolor sit amet, consectetur adipiscing elit. " * 5,
            "negative_int": -9876,
            "large_int": 9007199254740991,
            "small_float": 0.000001,
            "negative_float": -273.15
        },
        "nested_object": {
            "level1": {
                "level2": {
                    "level3": {
                        "deep_string": "deeply nested value",
                        "deep_number": 42,
                        "deep_array": [1, 2, 3]
                    }
                },
                "sibling": "sibling value"
            }
        },
        "arrays": {
            "int_array": list(range(1, 101)),
            "float_array": [i * 0.1 for i in range(100)],
            "string_array": [f"item_{i}" for i in range(50)],
            "bool_array": [i % 2 == 0 for i in range(20)],
            "object_array": [
                {"id": i, "name": f"object_{i}", "value": i * 1.5}
                for i in range(20)
            ]
        },
        "song": {
            "song_id": "TRAAABD128F429CF47",
            "title": "Soul Deep",
            "artist_name": "The Box Tops",
            "album": "Dimensions",
            "year": 1969,
            "duration_ms": 215000,
            "explicit": False,
            "popularity": 45,
            "genres": ["blue-eyed soul", "pop rock", "blues-rock"],
            "analysis": {
                "tempo": 120.5,
                "key": 5,
                "mode": 1,
                "time_signature": 4,
                "loudness": -5.2,
                "segments": [generate_segment(i) for i in range(num_segments)]
            }
        }
    }

    return data

def main():
    output_dir = os.path.dirname(os.path.abspath(__file__))

    # 生成不同大小的测试数据（目标文件大小）
    # 计算公式: 目标大小 ≈ 4.4KB + segments × 600B
    sizes = [
        (205, "128KB"),
        (420, "256KB"),
        (845, "512KB"),
        (1693, "1MB"),
        (3390, "2MB"),
        (8493, "5MB"),
        (16986, "10MB"),
        (33973, "20MB")
    ]

    for num_segments, size_name in sizes:
        print(f"Generating {size_name} with {num_segments} segments...")
        data = generate_test_data(num_segments)
        filename = f"test_data_{size_name}_{num_segments}.json"
        filepath = os.path.join(output_dir, filename)

        with open(filepath, 'w', encoding='utf-8') as f:
            json.dump(data, f, ensure_ascii=False)

        # 计算文件大小
        file_size = os.path.getsize(filepath)
        if file_size >= 1024 * 1024:
            print(f"Generated {filename}: {file_size / (1024 * 1024):.2f} MB")
        else:
            print(f"Generated {filename}: {file_size / 1024:.2f} KB")

if __name__ == "__main__":
    main()
