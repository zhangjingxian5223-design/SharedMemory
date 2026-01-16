import 'dart:convert';
import 'dart:io';
import 'package:flutter/material.dart';
import 'package:path_provider/path_provider.dart';
import 'benchmark_runner.dart';

/// Benchmark app main page
class BenchmarkApp extends StatefulWidget {
  const BenchmarkApp({Key? key}) : super(key: key);

  @override
  State<BenchmarkApp> createState() => _BenchmarkAppState();
}

class _BenchmarkAppState extends State<BenchmarkApp> {
  final BenchmarkRunner _runner = BenchmarkRunner();
  bool _isRunning = false;
  List<BenchmarkResult> _results = [];
  String _status = 'Ready';

  @override
  void initState() {
    super.initState();
    _initialize();
  }

  Future<void> _initialize() async {
    try {
      await _runner.initialize();
      setState(() {
        _status = 'Initialized - Ready to benchmark';
      });
    } catch (e) {
      setState(() {
        _status = 'Error: $e';
      });
    }
  }

  Future<void> _runBenchmark() async {
    setState(() {
      _isRunning = true;
      _status = 'Running benchmark...';
      _results.clear();
    });

    try {
      final results = await _runner.runAllBenchmarks();
      setState(() {
        _results = results;
        _status = 'Benchmark complete! ${results.length} tests run';
      });
    } catch (e) {
      setState(() {
        _status = 'Error: $e';
      });
    } finally {
      setState(() {
        _isRunning = false;
      });
    }
  }

  Future<void> _exportResults() async {
    try {
      final resultsJson = _results.map((r) => r.toJson()).toList();
      final jsonString = const JsonEncoder.withIndent('  ').convert(resultsJson);

      // Get application documents directory
      final directory = await getApplicationDocumentsDirectory();
      final file = File('${directory.path}/benchmark_results.json');

      // Write to file
      await file.writeAsString(jsonString);

      // Also save to temp directory for easy access
      final tempDir = await getTemporaryDirectory();
      final tempFile = File('${tempDir.path}/benchmark_results.json');
      await tempFile.writeAsString(jsonString);

      print('✅ Results saved to:');
      print('📁 ${file.path}');
      print('📁 ${tempFile.path}');

      setState(() {
        _status = 'Saved to: ${tempFile.path}';
      });
    } catch (e) {
      print('❌ Error saving results: $e');
      setState(() {
        _status = 'Error: $e';
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      home: Scaffold(
        appBar: AppBar(
          title: const Text('ShmProxy Benchmark'),
          backgroundColor: Colors.blue.shade700,
        ),
        body: GestureDetector(
          behavior: HitTestBehavior.opaque,
          onTap: () {
            // Dismiss keyboard when tapping anywhere
            FocusScope.of(context).requestFocus(FocusNode());
          },
          child: Column(
            children: [
              _buildStatusCard(),
              _buildControlButtons(),
              Expanded(
                child: _buildResultsList(),
              ),
            ],
          ),
        ),
      ),
    );
  }

  Widget _buildStatusCard() {
    return Card(
      margin: const EdgeInsets.all(16),
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              'Status',
              style: Theme.of(context).textTheme.titleMedium,
            ),
            const SizedBox(height: 8),
            Text(
              _status,
              style: const TextStyle(fontSize: 16),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildControlButtons() {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
      child: Row(
        children: [
          ElevatedButton(
            onPressed: _isRunning ? null : _runBenchmark,
            child: const Text('Run Benchmark'),
            style: ElevatedButton.styleFrom(
              backgroundColor: Colors.blue.shade700,
              foregroundColor: Colors.white,
            ),
          ),
          const SizedBox(width: 16),
          ElevatedButton(
            onPressed: _results.isEmpty ? null : _exportResults,
            child: const Text('Export Results'),
            style: ElevatedButton.styleFrom(
              backgroundColor: Colors.green.shade700,
              foregroundColor: Colors.white,
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildResultsList() {
    if (_results.isEmpty) {
      return const Center(
        child: Text(
          'No results yet. Run benchmark to see results.',
          style: TextStyle(fontSize: 16, color: Colors.grey),
        ),
      );
    }

    return ListView.builder(
      itemCount: _results.length,
      itemBuilder: (context, index) {
        final result = _results[index];
        return _buildResultCard(result);
      },
    );
  }

  Widget _buildResultCard(BenchmarkResult result) {
    // Calculate improvements
    final traditionalResult = _results.firstWhere(
      (r) => r.dataSize == result.dataSize &&
             r.accessType == result.accessType &&
             r.method == 'Traditional',
      );

    final jsonResult = _results.firstWhere(
      (r) => r.dataSize == result.dataSize &&
             r.accessType == result.accessType &&
             r.method == 'JSON String',
    );

    final shmImprovementVsTrad = traditionalResult.e2eMs > 0
        ? ((traditionalResult.e2eMs - result.e2eMs) / traditionalResult.e2eMs * 100)
        : 0.0;

    final shmImprovementVsJson = jsonResult.e2eMs > 0
        ? ((jsonResult.e2eMs - result.e2eMs) / jsonResult.e2eMs * 100)
        : 0.0;

    final isShmProxy = result.method == 'ShmProxy';
    final improvement = isShmProxy ? shmImprovementVsTrad : 0.0;

    return Card(
      margin: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              mainAxisAlignment: MainAxisAlignment.spaceBetween,
              children: [
                Text(
                  '${result.dataSize} - ${result.accessType}',
                  style: const TextStyle(
                    fontSize: 18,
                    fontWeight: FontWeight.bold,
                  ),
                ),
                _buildMethodChip(result.method),
              ],
            ),
            const SizedBox(height: 12),
            _buildPerformanceRow('Convert', result.convertMs),
            const SizedBox(height: 8),
            _buildPerformanceRow('Access', result.accessMs),
            const SizedBox(height: 8),
            _buildPerformanceRow('E2E', result.e2eMs, isBold: true),
            if (isShmProxy && improvement > 0) ...[
              const SizedBox(height: 12),
              _buildImprovementChip(improvement, shmImprovementVsJson),
            ],
          ],
        ),
      ),
    );
  }

  Widget _buildMethodChip(String method) {
    Color color;
    switch (method) {
      case 'Traditional':
        color = Colors.grey;
        break;
      case 'JSON String':
        color = Colors.blue;
        break;
      case 'ShmProxy':
        color = Colors.green;
        break;
      default:
        color = Colors.grey;
    }

    return Chip(
      label: Text(method),
      backgroundColor: color.withOpacity(0.1),
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
    );
  }

  Widget _buildPerformanceRow(String label, double valueMs, {bool isBold = false}) {
    return Row(
      mainAxisAlignment: MainAxisAlignment.spaceBetween,
      children: [
        Text(
          label,
          style: TextStyle(
            fontSize: 14,
            fontWeight: isBold ? FontWeight.bold : FontWeight.normal,
          ),
        ),
        Text(
          '${valueMs.toStringAsFixed(2)} ms',
          style: TextStyle(
            fontSize: 14,
            fontWeight: isBold ? FontWeight.bold : FontWeight.normal,
          ),
        ),
      ],
    );
  }

  Widget _buildImprovementChip(double vsTraditional, double vsJson) {
    return Row(
      children: [
        _buildImprovement('vs Traditional', vsTraditional, Colors.green),
        const SizedBox(width: 8),
        _buildImprovement('vs JSON String', vsJson, Colors.blue),
      ],
    );
  }

  Widget _buildImprovement(String label, double improvement, Color color) {
    final text = '${improvement.toStringAsFixed(1)}% faster';
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
      decoration: BoxDecoration(
        color: color.withOpacity(0.1),
        borderRadius: BorderRadius.circular(16),
      ),
      child: Text(
        '$label: $text',
        style: TextStyle(
          fontSize: 12,
          color: color.withOpacity(0.7),
          fontWeight: FontWeight.bold,
        ),
      ),
    );
  }
}
