# ShmProxyLazy.podspec
#
# Lazy-loading data transfer using ES6 Proxy and Shared Memory for React Native
#
# Copyright (c) 2024
# Licensed under the MIT license

Pod::Spec.new do |s|
  s.name           = 'react-native-shmproxy-lazy'
  s.version        = '1.0.0'
  s.summary        = 'Lazy-loading data transfer using ES6 Proxy and Shared Memory for React Native'
  s.description    = <<-DESC
    ShmProxyLazy uses ES6 Proxy to intercept property access and only convert data
    from shared memory when fields are actually accessed.

    Performance improvements:
    - 1MB data (partial access): ~65% faster than traditional method
    - 20MB data (partial access): ~67% faster than traditional method

    Features:
    - ES6 Proxy for lazy loading
    - Zero-copy data transfer
    - Direct JSI bindings for synchronous access
    - Field-level caching
    - Full TypeScript support
    - iOS ready (Android support planned)
                   DESC

  s.homepage     = 'https://github.com/yourusername/react-native-shmproxy'
  s.license      = { :type => 'MIT', :file => 'LICENSE' }
  s.author       = { 'Your Name' => 'your.email@example.com' }
  s.platforms     = { :ios => '11.0' }
  s.source       = { :git => 'https://github.com/yourusername/react-native-shmproxy.git', :tag => "#{s.version}" }

  # Native module dependencies
  s.dependency 'React-Core'
  s.dependency 'React-RCTBridge'
  s.dependency 'React-jsi'
  s.dependency 'React-RCTAPI'
  s.dependency 'react-native-shmproxy'

  # Source files
  s.source_files = 'ios/ShmProxyLazy/**/*.{h,mm,cpp}'

  # Header search paths
  s.pod_target_xcconfig = {
    'HEADER_SEARCH_PATHS' => [
      '"$(PODS_TARGET_SRCROOT)"',
      '"$(PODS_ROOT)/React-Core/ReactCommon/jsi"',
      '"$(PODS_ROOT)/react-native-shmproxy/ios/ShmProxy"',
    ],
    'CLANG_CXX_LANGUAGE_STANDARD' => 'c++17',
    'CLANG_CXX_LIBRARY' => 'libc++',
  }

  # Compiler flags
  s.compiler_flags = '-DFOLLY_NO_CONFIG'

  # Ensure all required frameworks are linked
  s.frameworks = 'UIKit', 'Foundation'

  # Swift version (if using Swift in the future)
  s.swift_version = '5.0'

  # If you need to exclude certain files, use this:
  # s.exclude_files = 'ios/**/*Tests.{h,mm}'

  # Private header files (if needed)
  # s.private_header_files = 'ios/ShmProxyLazy/Private/*.h'

end
