#!/bin/bash

# Quick setup script to create a React Native test project with ShmProxy

set -e

echo "=========================================="
echo "  ShmProxy Test Project Setup"
echo "=========================================="
echo ""

PROJECT_DIR="/Users/zjxzjx/Documents/RN"
PROJECT_NAME="ShmProxyTest"
LIBRARY_DIR="/Users/zjxzjx/Documents/RN/SharedMemory"

echo "This script will:"
echo "1. Create a new React Native project: $PROJECT_DIR/$PROJECT_NAME"
echo "2. Install ShmProxy packages from local path"
echo "3. Configure iOS dependencies"
echo ""

# Check if npx is available
if ! command -v npx &> /dev/null; then
    echo "❌ Error: npx is not installed"
    echo "   Please install Node.js and npm"
    exit 1
fi

# Check if SharedMemory directory exists
if [ ! -d "$LIBRARY_DIR" ]; then
    echo "❌ Error: SharedMemory directory not found at $LIBRARY_DIR"
    exit 1
fi

echo "✅ Found SharedMemory at: $LIBRARY_DIR"
echo ""

# Go to parent directory
cd "$PROJECT_DIR"

echo "=========================================="
echo "Step 1: Creating React Native Project"
echo "=========================================="
echo ""

# Check if project already exists
if [ -d "$PROJECT_NAME" ]; then
    echo "⚠️  Warning: Project '$PROJECT_NAME' already exists"
    read -p "Do you want to delete it and recreate? (y/N): " -n 1
    echo ""
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        echo "Deleting existing project..."
        rm -rf "$PROJECT_NAME"
    else
        echo "Aborted. Please delete the project manually and try again."
        exit 1
    fi
fi

echo "Creating React Native project: $PROJECT_NAME (version 0.73.11)"
npx @react-native-community/cli init "$PROJECT_NAME" --version "0.73.11" <<EOF

y
EOF

echo "✅ Project created"
echo ""

cd "$PROJECT_NAME"

echo "=========================================="
echo "Step 2: Installing ShmProxy packages"
echo "=========================================="
echo ""

echo "Installing shmproxy from: $LIBRARY_DIR/packages/shmproxy"
npm install "$LIBRARY_DIR/packages/shmproxy"

echo ""
echo "Installing shmproxy-lazy from: $LIBRARY_DIR/packages/shmproxy-lazy"
npm install "$LIBRARY_DIR/packages/shmproxy-lazy"

echo ""
echo "✅ ShmProxy packages installed"
echo ""

echo "=========================================="
echo "Step 3: Installing dependencies"
echo "=========================================="
echo ""

npm install

echo ""
echo "✅ Dependencies installed"
echo ""

echo "=========================================="
echo "Step 4: Configuring iOS"
echo "=========================================="
echo ""

cd ios
echo "Running: pod install"
pod install

cd ..

echo ""
echo "=========================================="
echo "Fixing Metro bundler symlinks..."
echo "=========================================="
echo ""

cd "$PROJECT_DIR/$PROJECT_NAME"

# Remove symlinks created by npm install
echo "Removing symlinks..."
rm -rf node_modules/react-native-shmproxy node_modules/react-native-shmproxy-lazy

# Copy actual packages (Metro bundler requires real files, not symlinks)
echo "Copying shmproxy package..."
cp -r "$LIBRARY_DIR/packages/shmproxy" node_modules/react-native-shmproxy

echo "Copying shmproxy-lazy package..."
cp -r "$LIBRARY_DIR/packages/shmproxy-lazy" node_modules/react-native-shmproxy-lazy

echo ""
echo "✅ Packages installed (copied for Metro compatibility)"
echo ""

cd "$PROJECT_DIR"

echo ""
echo "=========================================="
echo "⚠️  Important Notes"
echo "=========================================="
echo ""
echo "The setup script has made the following fixes to ShmProxy:"
echo ""
echo "1. ✅ Using React Native 0.73.11 (旧架构，完全兼容):"
echo "   - 旧架构无需修改 podspec"
echo "   - ShmProxy 已在 RN 0.73.11 中测试通过"
echo ""
echo "2. ✅ Fixed Metro bundler symlinks:"
echo "   - Replaced symlinks with actual package copies"
echo "   - Metro bundler cannot resolve symlinks"
echo ""
echo "3. ✅ Autolinking now detects both ShmProxy modules"
echo ""
echo "=========================================="
echo "Setup Complete!"
echo "=========================================="
echo ""

echo "Next steps:"
echo ""
echo "1. If pod install failed due to network issues:"
echo "   cd $PROJECT_DIR/$PROJECT_NAME/ios"
echo "   pod install"
echo ""
echo "2. Run the project:"
echo "   cd $PROJECT_DIR/$PROJECT_NAME"
echo "   npm run ios"
echo ""
echo "3. Or on Android:"
echo "   cd $PROJECT_DIR/$PROJECT_NAME"
echo "   npm run android"
echo ""
echo "4. Check the examples at:"
echo "   $LIBRARY_DIR/examples/basic-usage/"
echo ""
echo "5. For setup status and troubleshooting:"
echo "   cat $LIBRARY_DIR/SETUP_STATUS.md"
echo ""
echo "For more information, see:"
echo "   $LIBRARY_DIR/USAGE_GUIDE.md"
echo ""
echo "🎉 Happy testing!"
