# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Initial release of ShmProxy and ShmProxyLazy modules
- High-performance data transfer using Shared Memory for React Native
- 55-68% performance improvement over traditional Bridge method
- Full TypeScript support
- ES6 Proxy-based lazy loading (ShmProxyLazy)
- Complete API documentation
- iOS support (iOS 11.0+)

### Features
- **ShmProxy**: Full data conversion with 55-60% performance improvement
  - `write()` - Write JavaScript object to shared memory
  - `read()` - Read object with full conversion
  - `getStats()` - Get shared memory statistics
  - `clear()` - Clear shared memory
  - JSI synchronous functions: `__shm_write`, `__shm_read`, `__shm_getStats`, `__shm_clear`

- **ShmProxyLazy**: Lazy loading with 64-68% performance improvement (partial access)
  - `write()` - Write JavaScript object to shared memory
  - `createProxy()` - Create ES6 Proxy for lazy access
  - `materialize()` - Full object conversion
  - Field-level caching
  - Nested object support

### Performance
- 1MB data: 65% faster (partial access), 44% faster (full access)
- 20MB data: 67% faster (partial access), 45% faster (full access)
- Zero-copy data transfer using Shared Memory
- Avoids traditional Bridge serialization overhead

### Documentation
- Installation guide
- API reference
- Basic usage example
- Podfile integration guide
- Architecture documentation
- Performance analysis

## [1.0.0] - 2026-01-21

### Added
- Initial release
- ShmProxy module for high-performance data transfer
- ShmProxyLazy module for lazy loading
- Complete TypeScript support
- iOS support
- Comprehensive documentation
- Example projects

### Features
- Shared Memory-based data transfer
- JSI synchronous bindings
- ES6 Proxy-based lazy loading
- Field-level caching
- Type-safe API

### Performance
- 55-68% faster than traditional Bridge method
- Zero-copy data transfer
- Lazy loading support

### Known Limitations
- iOS only (Android support planned)
- Requires React Native 0.60+
- Requires iOS 11.0+

---

## Version Format

The version format is: `MAJOR.MINOR.PATCH`

- **MAJOR**: Incompatible API changes
- **MINOR**: Backwards-compatible functionality additions
- **PATCH**: Backwards-compatible bug fixes

## Release Notes Template

```markdown
## [1.x.x] - YYYY-MM-DD

### Added
- New feature description

### Changed
- Modified feature description

### Deprecated
- Feature that will be removed in future versions

### Removed
- Feature that was removed

### Fixed
- Bug fix description

### Security
- Security fix description
```

---

**For more information, see:**
- [README](./README.md)
- [API Reference](./docs/api-reference.md)
- [Contributing](./CONTRIBUTING.md)
