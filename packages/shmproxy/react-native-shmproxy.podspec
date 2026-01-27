# ShmProxy.podspec
#
# High-performance data transfer using Shared Memory for React Native
#
# Copyright (c) 2024
# Licensed under the MIT license

Pod::Spec.new do |s|
  s.name           = 'react-native-shmproxy'
  s.version        = '1.0.0'
  s.summary        = 'High-performance data transfer using Shared Memory for React Native'
  s.description    = <<-DESC
    ShmProxy provides faster data transfer between Native and JavaScript by using
    shared memory instead of the traditional bridge serialization.

    Performance improvements:
    - 1MB data: ~57% faster than traditional method
    - 20MB data: ~57% faster than traditional method

    Features:
    - Zero-copy data transfer
    - Direct JSI bindings for synchronous access
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
  s.dependency 'React-jsi'

  # Source files
  s.source_files = 'ios/ShmProxy/**/*.{h,mm,cpp}'

  # Header search paths
  s.pod_target_xcconfig = {
    'HEADER_SEARCH_PATHS' => [
      '"$(PODS_TARGET_SRCROOT)"',
      '"$(PODS_ROOT)/React-Core/ReactCommon/jsi"',
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
  # s.private_header_files = 'ios/ShmProxy/Private/*.h'

end
