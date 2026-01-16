#
# To learn more about a Podspec see http://guides.cocoapods.org/syntax/podspec.html.
# Run `pod lib lint shm_flutter_new.podspec` to validate before publishing.
#
Pod::Spec.new do |s|
  s.name             = 'shm_proxy'
  s.version          = '0.0.1'
  s.summary          = 'High-performance Native-Dart data transfer plugin using shared memory.'
  s.description      = <<-DESC
SharedMemory plugin for Flutter enables zero-copy data transfer between Native and Dart layers using shared memory.
Achieves 55-60% performance improvement over traditional MethodChannel communication.
                       DESC
  s.homepage         = 'https://github.com/zhangjingxian5223-design/SharedMemory'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'zhangjingxian5223' => 'zhangjingxian5223@gmail.com' }
  s.source           = { :path => '.' }
  s.source_files = 'Classes/**/*'
  s.resources = ['TestData/*.json']
  s.dependency 'Flutter'
  s.dependency 'OpenSSL-Universal'
  s.platform = :ios, '13.0'

  # Flutter.framework does not contain a i386 slice.
  s.pod_target_xcconfig = { 'DEFINES_MODULE' => 'YES', 'EXCLUDED_ARCHS[sdk=iphonesimulator*]' => 'i386' }
  s.swift_version = '5.0'

  # If your plugin requires a privacy manifest, for example if it uses any
  # required reason APIs, update the PrivacyInfo.xcprivacy file to describe your
  # plugin's privacy impact, and then uncomment this line. For more information,
  # see https://developer.apple.com/documentation/bundleresources/privacy_manifest_files
  # s.resource_bundles = {'shm_proxy_privacy' => ['Resources/PrivacyInfo.xcprivacy']}
end
