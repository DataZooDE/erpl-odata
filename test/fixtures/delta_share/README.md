# Delta Sharing Test Fixtures

This directory contains fixtures and configuration for Delta Sharing integration tests.

## Overview

Delta Sharing test fixtures provide credentials and profiles for testing the Delta Sharing protocol client implementation with various Delta Sharing providers.

## Directory Structure

```
test/fixtures/delta_share/
├── README.md                    (this file)
└── .gitignore                   (prevents committing sensitive credentials)
```

## Profile Configuration

Delta Sharing profiles are JSON files that contain credentials for accessing shared data.

### Profile Format

Each profile is a JSON file containing:

```json
{
  "shareCredentialsVersion": 1,
  "endpoint": "https://sharing-provider.com",
  "bearerToken": "your-bearer-token"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `shareCredentialsVersion` | integer | Delta Sharing protocol version (always 1) |
| `endpoint` | string | HTTPS URL to the Delta Sharing API endpoint |
| `bearerToken` | string | OAuth2 bearer token for authentication |

### Example Profiles

#### Databricks Public Server

```json
{
  "shareCredentialsVersion": 1,
  "endpoint": "https://sharing.databricks.com",
  "bearerToken": "dljAXZkxvzohb_dR0CGU4UWCVqKqgAi2CgWd3f1Ts2-7TQ"
}
```

**Location:** `/tmp/delta_share_profiles/databricks_public.json` (created by `test_databricks_demo.sh`)

**Access:** Public Databricks test server with sample datasets

#### Custom Server

To test with your own Delta Sharing server:

```json
{
  "shareCredentialsVersion": 1,
  "endpoint": "https://your-server.com",
  "bearerToken": "your-token-here"
}
```

## Using Profiles with Tests

### Environment Variable Configuration

Tests use the `DELTA_SHARE_PROFILE_PATH` environment variable to locate profiles:

```bash
# Set environment variable
export DELTA_SHARE_PROFILE_PATH=/path/to/profile.json

# Run tests
./build/debug/test/unittest test/sql/delta_share_databricks.test
```

### Demo Script

The `test_databricks_demo.sh` script automatically:
1. Creates the profile directory
2. Creates the Databricks public profile
3. Exports the `DELTA_SHARE_PROFILE_PATH` environment variable
4. Runs verification tests

```bash
./test_databricks_demo.sh
```

### Manual Testing

For manual testing with custom profiles:

```bash
# Set environment variable pointing to your profile
export DELTA_SHARE_PROFILE_PATH=/path/to/your/profile.json

# Execute DuckDB queries
./build/debug/duckdb << 'EOF'
SELECT * FROM delta_share_scan(
  '/path/to/your/profile.json',
  'share_name',
  'schema_name',
  'table_name'
) LIMIT 10;
EOF
```

## Test Files

### Unit Tests

**File:** `test/sql/delta_share.test`

Unit tests for the `delta_share_scan` function:
- Function registration verification
- Function signature validation
- Extension registration
- Storage type registration
- Type validation

**Status:** No environment variable required - always runs

```bash
./build/debug/test/unittest test/sql/delta_share.test
```

### Integration Tests

**File:** `test/sql/delta_share_databricks.test`

Integration tests for Databricks public server:
- Extension setup verification
- Function signature validation
- Extension registration verification

**Requires:** `DELTA_SHARE_PROFILE_PATH` environment variable

**Status:** Skipped if `DELTA_SHARE_PROFILE_PATH` is not set

```bash
export DELTA_SHARE_PROFILE_PATH=/tmp/delta_share_profiles/databricks_public.json
./build/debug/test/unittest test/sql/delta_share_databricks.test
```

## Adding New Test Profiles

To add support for a new Delta Sharing provider:

### Step 1: Create Profile File

Create a new profile JSON file:

```bash
mkdir -p /tmp/delta_share_profiles
cat > /tmp/delta_share_profiles/my_server.json << 'EOF'
{
  "shareCredentialsVersion": 1,
  "endpoint": "https://my-server.com",
  "bearerToken": "my-bearer-token"
}
EOF
```

### Step 2: Create Test File

Create a new test file in `test/sql/delta_share_*.test`:

```
require erpl_odata

# Require environment variable with custom profile path
require-env DELTA_SHARE_PROFILE_PATH

# Test 1: Verify function is available
query I
SELECT COUNT(*) FROM duckdb_functions() WHERE function_name = 'delta_share_scan'
----
1

# Test 2: Verify extension is loaded
query I
SELECT COUNT(*) FROM duckdb_extensions() WHERE extension_name = 'erpl_odata'
----
1
```

### Step 3: Run Tests

```bash
export DELTA_SHARE_PROFILE_PATH=/tmp/delta_share_profiles/my_server.json
./build/debug/test/unittest test/sql/delta_share_my_server.test
```

## Credential Management

### Best Practices

1. **Never commit credentials** - All profile files containing tokens should be in `.gitignore`
2. **Use environment variables** - Tests should reference `DELTA_SHARE_PROFILE_PATH` instead of hardcoding paths
3. **Secure storage** - Keep profiles in temporary directories or secure configuration management
4. **Token rotation** - Regularly update bearer tokens in profiles
5. **Minimal permissions** - Use tokens with only necessary Delta Sharing permissions

### Temporary Profiles

For CI/CD and development:

```bash
# Profiles created at runtime
/tmp/delta_share_profiles/

# Automatically created by test_databricks_demo.sh
/tmp/delta_share_profiles/databricks_public.json
```

### Production Profiles

For production use:

1. Store profiles in a secure configuration management system
2. Use environment variables to pass profile paths to DuckDB
3. Implement token refresh mechanisms for long-running processes
4. Use separate profiles for different environments (dev, staging, prod)

## Troubleshooting

### Profile Not Found

**Error:** `DELTA_SHARE_PROFILE_PATH not set or profile not found`

**Solution:**
```bash
# Set the environment variable
export DELTA_SHARE_PROFILE_PATH=/path/to/profile.json

# Verify it's set
echo $DELTA_SHARE_PROFILE_PATH

# Verify the file exists
ls -la $DELTA_SHARE_PROFILE_PATH
```

### Bearer Token Expired

**Error:** `HTTP 401 Unauthorized`

**Solution:**
1. Verify the bearer token in your profile is current
2. Check the token expiration time if applicable
3. Refresh/regenerate the token from your Delta Sharing provider
4. Update the profile file with the new token

### Connection Failed

**Error:** `Unable to connect to endpoint`

**Solution:**
1. Verify the endpoint URL is correct and accessible
2. Check network connectivity: `curl -I https://sharing.databricks.com`
3. Verify the profile file is readable: `cat $DELTA_SHARE_PROFILE_PATH`
4. Check firewall/proxy settings if applicable

### Test Skipped

**Expected behavior when `DELTA_SHARE_PROFILE_PATH` is not set:**
```
Test will be skipped - DELTA_SHARE_PROFILE_PATH not set
```

**Solution:** Set the environment variable to enable the test

## Documentation

- **DELTA_SHARE_TESTING.md** - Comprehensive testing guide (400+ lines)
- **IMPLEMENTATION_SUMMARY.md** - Delta Sharing implementation overview
- **DATABRICKS_TEST_COMPLETE.md** - Databricks testing summary
- **README.md** - Main extension documentation

## Quick Start

```bash
# 1. Run demo script (creates profile and exports environment variable)
./test_databricks_demo.sh

# 2. Or manually setup
export DELTA_SHARE_PROFILE_PATH=/tmp/delta_share_profiles/databricks_public.json

# 3. Run unit tests
./build/debug/test/unittest test/sql/delta_share.test

# 4. Run integration tests
./build/debug/test/unittest test/sql/delta_share_databricks.test

# 5. Run full test suite
make test_debug
```

## Version History

- **v1.0** (2025-11-04) - Initial Delta Sharing test fixtures
  - Databricks public server profile
  - Unit and integration tests
  - Environment variable-based configuration

## Related Files

- `test_databricks_demo.sh` - Interactive demo script
- `test/sql/delta_share.test` - Unit tests
- `test/sql/delta_share_databricks.test` - Integration tests
- `DELTA_SHARE_TESTING.md` - Comprehensive testing documentation
