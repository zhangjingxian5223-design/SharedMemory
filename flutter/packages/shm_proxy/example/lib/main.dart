import 'package:flutter/material.dart';
import 'package:shm_proxy/src/benchmark_app.dart';

void main() {
  runApp(const MyApp());
}

class MyApp extends StatelessWidget {
  const MyApp({super.key});

  @override
  Widget build(BuildContext context) {
    return const MaterialApp(
      title: 'ShmProxy Benchmark',
      home: BenchmarkApp(),
    );
  }
}
