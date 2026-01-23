# Podfile Integration Guide

This guide explains how to integrate ShmProxy and ShmProxyLazy modules into your React Native iOS project.

## Automatic Integration (React Native >= 0.60)

If you're using React Native 0.60 or higher with CocoaPods autolinking, no manual Podfile changes are required.

Simply install the packages:

```bash
npm install react-native-shmproxy
npm install react-native-shmproxy-lazy
cd ios && pod install
```

## Manual Integration

If you need to manually integrate the pods, follow these steps:

### 1. Update your Podfile

Add the following to your iOS `Podfile`:

```ruby
platform :ios, '11.0'

target 'YourProject' do
  # Existing React Native dependencies
  config = use_native_modules!

  # ShmProxy modules
  pod 'react-native-shmproxy', :path => '../node_modules/react-native-shmproxy'
  pod 'react-native-shmproxy-lazy', :path => '../node_modules/react-native-shmproxy-lazy'

  # Use the genberated pods
  target 'YourProjectTests' do
    inherit! :search_paths
  end
end
```

### 2. Install Pods

```bash
cd ios
pod install
```

### 3. Open Workspace

After installing pods, open the `.xcworkspace` file (not the `.xcodeproj`):

```bash
open YourProject.xcworkspace
```

## Monorepo Integration

If you're developing in a monorepo with local packages:

```ruby
# Podfile
platform :ios, '11.0'

target 'YourProject' do
  config = use_native_modules!

  # Local packages (for development)
  pod 'react-native-shmproxy', :path => '../../packages/shmproxy'
  pod 'react-native-shmproxy-lazy', :path => '../../packages/shmproxy-lazy'
end
```

## Verification

### Check Pod Installation

```bash
cd ios
pod install
```

You should see output similar to:

```
Analyzing dependencies
Installing react-native-shmproxy (1.0.0)
Installing react-native-shmproxy-lazy (1.0.0)
...
Pod installation complete!
```

### Verify in Xcode

1. Open your project workspace
2. Navigate to `Pods` → `Development Pods`
3. You should see:
   - `react-native-shmproxy`
   - `react-native-shmproxy-lazy`

## Troubleshooting

### Issue: "Pod not found"

**Solution**: Make sure the `.podspec` files exist in the correct locations:
- `node_modules/react-native-shmproxy/ShmProxy.podspec`
- `node_modules/react-native-shmproxy-lazy/ShmProxyLazy.podspec`

### Issue: "Compilation errors"

**Solution**: Clean and reinstall:

```bash
cd ios
rm -rf Pods Podfile.lock
pod deintegrate
pod install
```

### Issue: "Linker errors"

**Solution**: Add the following to your Podfile:

```ruby
post_install do |installer|
  installer.pods_project.targets.each do |target|
    target.build_configurations.each do |config|
      config.build_settings['HEADER_SEARCH_PATHS'] ||= ['$(inherited)']
      config.build_settings['HEADER_SEARCH_PATHS'] << '"${PODS_ROOT}/React-Core/ReactCommon/jsi"'
    end
  end
end
```

## Podfile Configuration Options

### Minimum iOS Version

Both modules require iOS 11.0 or higher:

```ruby
platform :ios, '11.0'
```

### C++ Standard

The modules use C++17:

```ruby
post_install do |installer|
  installer.pods_project.targets.each do |target|
    target.build_configurations.each do |config|
      config.build_settings['CLANG_CXX_LANGUAGE_STANDARD'] = 'c++17'
      config.build_settings['CLANG_CXX_LIBRARY'] = 'libc++'
    end
  end
end
```

### Frameworks

Required frameworks:

```ruby
target 'YourProject' do
  # ... other pods ...

  # Required frameworks
  s.frameworks = 'UIKit', 'Foundation'
end
```

## Next Steps

After pod installation:

1. **Build your project** in Xcode
2. **Install JSI bindings** in your JavaScript code
3. **Test the modules** using the example code

See:
- [Installation Guide](../docs/installation.md)
- [Basic Usage Example](../examples/basic-usage/)

## Additional Resources

- [CocoaPods Documentation](https://cocoapods.org/)
- [React Native Native Modules](https://reactnative.dev/docs/next/native-modules-intro)
- [Podspec Reference](https://guides.cocoapods.org/syntax/podspec.html)
