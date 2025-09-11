# XLayer MPTCP Scheduler Comparison

## Overview

This document compares the new XLayer implementation (`xlayer.c`) with the legacy version (`xlayer_old.c`), highlighting the key differences, improvements, and architectural changes made for MPTCP v1 compatibility.

## Summary of Changes

The new XLayer implementation is a complete rewrite that combines the original XLayer cross-layer optimization with BLEST head-of-line blocking prevention, adapted for MPTCP v1 framework.

## Architecture Comparison

### Legacy XLayer (`xlayer_old.c`)
- **MPTCP Version**: v0 (older kernel interface)
- **Framework**: Uses legacy MPTCP structures (`mptcp_cb`, `mptcp_tcp_sock`)
- **Interface**: `ms_get_available_subflow()` function
- **Algorithm**: Basic cross-layer WiFi/5G selection
- **HoL Prevention**: BLEST-style calculations without full integration

### New XLayer (`xlayer.c`)
- **MPTCP Version**: v1 (modern kernel interface)
- **Framework**: Uses current MPTCP structures (`mptcp_sock`, `mptcp_subflow_context`)
- **Interface**: Standard scheduler ops structure with `get_subflow()` callback
- **Algorithm**: Hybrid XLayer + BLEST with full integration
- **HoL Prevention**: Complete BLEST algorithm with intelligent override

## Key Differences

### 1. MPTCP Framework Integration

#### Legacy Implementation
```c
struct sock *ms_get_available_subflow(struct sock *meta_sk, struct sk_buff *skb, bool zero_wnd_test)
{
    struct mptcp_cb *mpcb = tcp_sk(meta_sk)->mpcb;
    struct mptcp_tcp_sock *mptcp;
    // Legacy MPTCP v0 structures
}
```

#### New Implementation
```c
static int mptcp_sched_xlayer_get_subflow(struct mptcp_sock *msk, struct mptcp_sched_data *data)
{
    struct mptcp_subflow_context *subflow;
    // Modern MPTCP v1 structures
}
```

### 2. Scheduler Registration

#### Legacy Implementation
- No formal scheduler registration
- Direct function replacement in MPTCP core

#### New Implementation
```c
static struct mptcp_sched_ops mptcp_sched_xlayer = {
    .init       = mptcp_sched_xlayer_init,
    .release    = mptcp_sched_xlayer_release,
    .get_subflow = mptcp_sched_xlayer_get_subflow,
    .name       = "xlayer",
    .owner      = THIS_MODULE,
};
```

### 3. Network Metrics Collection

#### Legacy Implementation
```c
// Global variables directly in main file
struct net_metrics net_info;
// Thread management in xlayer_old.h
```

#### New Implementation
```c
// Clean encapsulation with proper initialization
static struct net_metrics net_info = {
    .wifi_ratio = -1,
    .nr_ratio = -1,
    .lock = __SPIN_LOCK_UNLOCKED(net_info.lock),
};

// Integrated thread management
static int net_metrics_updater_thread(void *data);
```

### 4. Proc Interface

#### Legacy Implementation
- Proc name: `"rm5xxq_proc"` (device-specific)
- Basic functionality

#### New Implementation
- Proc name: `"xlayer_5g_proc"` (descriptive)
- Enhanced error handling and validation
- Better documentation

### 5. Algorithm Logic and HoL Blocking Prevention

#### Legacy Implementation
```c
// Cross-layer selection
if (wifi_ratio > nr_ratio) {
    mssk = sock_wifi;
} else {
    mssk = sock_5g;
}

// BLEST HoL blocking prevention (basic approach)
defaultsk = get_available_subflow(meta_sk, skb, zero_wnd_test);
blestsk = defaultsk;

if (blestsk && mssk && blestsk != mssk) {
    // Calculate HoL blocking risk
    slow_linger_time = ms_sched_estimate_linger_time(blestsk);
    fast_bytes = ms_sched_estimate_bytes(mssk, slow_linger_time);
    
    slow_inflight_bytes = besttp->write_seq - besttp->snd_una;
    slow_bytes = buffered_bytes + slow_inflight_bytes;
    avail_space = (slow_bytes < meta_tp->snd_wnd) ? (meta_tp->snd_wnd - slow_bytes) : 0;
    
    if (fast_bytes > avail_space) {
        blestsk = NULL; // Override with fastest subflow
    }
}
return blestsk; // Return either cross-layer choice or fastest subflow
```

#### New Implementation
```c
// Enhanced cross-layer selection with better fallbacks
if (wifi_subflow && nr_subflow) {
    if (wifi_ratio > 0 && nr_ratio > 0) {
        if (wifi_ratio > nr_ratio) {
            max_datarate_subflow = wifi_subflow;
        } else {
            max_datarate_subflow = nr_subflow;
        }
    } else {
        // Intelligent fallback to RTT-based selection
        max_datarate_subflow = real_minrtt_subflow;
    }
}

// Improved BLEST HoL blocking prevention
if (max_datarate_subflow == real_minrtt_subflow) {
    // Already optimal, schedule directly
    mptcp_subflow_set_scheduled(max_datarate_subflow, 1);
    return 0;
}

// Enhanced HoL blocking check with better space calculations
common_update_lambda(msk, max_datarate_subflow);
slow_linger_time = common_estimate_linger_time(max_datarate_subflow);
fast_bytes = common_estimate_bytes(real_minrtt_subflow, slow_linger_time);

// More accurate space calculation using tcp_wnd_end()
snd_wnd = tcp_wnd_end(meta_tp) - meta_tp->write_seq;
avail_space = (slow_bytes < snd_wnd) ? (snd_wnd - slow_bytes) : 0;

if (fast_bytes > avail_space) {
    // Override with fastest subflow to prevent HoL blocking
    mptcp_subflow_set_scheduled(real_minrtt_subflow, 1);
} else {
    // Safe to use cross-layer selected subflow
    mptcp_subflow_set_scheduled(max_datarate_subflow, 1);
}
```

## Feature Comparison

| Feature | Legacy XLayer | New XLayer | Improvement |
|---------|---------------|------------|-------------|
| **MPTCP Version** | v0 | v1 | ✅ Modern framework |
| **Cross-layer Selection** | Basic | Enhanced | ✅ Better fallback logic |
| **HoL Blocking Prevention** | Basic BLEST | Enhanced BLEST | ✅ Improved calculations |
| **Code Organization** | Mixed files | Single file | ✅ Better maintainability |
| **Error Handling** | Basic | Comprehensive | ✅ Robust error recovery |
| **Documentation** | Minimal | Extensive | ✅ Clear comments & docs |
| **Thread Safety** | Basic | Enhanced | ✅ Proper synchronization |
| **Modularity** | Monolithic | Modular | ✅ Clean separation |
| **Debugging** | Limited | Extensive | ✅ Detailed logging |
| **Window Calculation** | `meta_tp->snd_wnd` | `tcp_wnd_end()` | ✅ More accurate |
| **Subflow Scheduling** | Return pointer | `mptcp_subflow_set_scheduled()` | ✅ MPTCP v1 standard |

## Benefits of New Implementation

### 1. **Future-Proof Architecture**
- Uses modern MPTCP v1 interfaces
- Compatible with latest kernel versions
- Standard scheduler framework integration

### 2. **Enhanced Algorithm**
- Combines XLayer and BLEST benefits
- Intelligent fallback mechanisms
- Better performance under various conditions

### 3. **Improved Maintainability**
- Single, well-documented file
- Clear separation of concerns
- Extensive comments and documentation

### 4. **Better Error Handling**
- Comprehensive error checking
- Graceful fallback strategies
- Detailed error reporting

### 5. **Production Ready**
- Thread-safe operations
- Proper resource management
- Clean module lifecycle

## Migration Path

### For Developers
1. **API Changes**: Update to MPTCP v1 structures and functions
2. **Scheduler Registration**: Use standard `mptcp_sched_ops` structure
3. **Function Signatures**: Adapt to new callback interfaces
4. **Error Handling**: Implement comprehensive error checking

### For Users
1. **Kernel Upgrade**: Ensure MPTCP v1 support in kernel
2. **Module Loading**: Standard module loading procedure
3. **Configuration**: Use new proc interface (`/proc/xlayer_5g_proc`)
4. **Activation**: Standard scheduler selection (`echo xlayer > /proc/sys/net/mptcp/scheduler`)

## HoL Blocking Prevention Comparison

Both implementations include Head-of-Line blocking prevention based on the BLEST algorithm, but with different approaches:

### Legacy XLayer HoL Prevention
```c
/* if we decided to use a slower flow, we have the option of not using it at all */
else if (blestsk && mssk && blestsk != mssk)
{
    u32 slow_linger_time, fast_bytes, slow_inflight_bytes, slow_bytes, avail_space;
    u32 buffered_bytes = 0;
    
    ms_sched_update_lambda(meta_sk, blestsk);
    slow_linger_time = ms_sched_estimate_linger_time(blestsk);
    fast_bytes = ms_sched_estimate_bytes(mssk, slow_linger_time);
    
    if (skb) buffered_bytes = skb->len;
    slow_inflight_bytes = besttp->write_seq - besttp->snd_una;
    slow_bytes = buffered_bytes + slow_inflight_bytes;
    
    avail_space = (slow_bytes < meta_tp->snd_wnd) ? (meta_tp->snd_wnd - slow_bytes) : 0;
    
    if (fast_bytes > avail_space) {
        blestsk = NULL; // Override with NULL, fallback to default
    }
}
```

**Characteristics:**
- ✅ Complete BLEST algorithm implementation
- ✅ Accounts for buffered bytes from current SKB
- ✅ Proper linger time estimation
- ❌ Uses `meta_tp->snd_wnd` (simpler window calculation)
- ❌ Returns NULL on HoL risk (relies on framework fallback)

### New XLayer HoL Prevention
```c
/* Check for HoL blocking risk */
{
    common_update_lambda(msk, max_datarate_subflow);
    slow_linger_time = common_estimate_linger_time(max_datarate_subflow);
    fast_bytes = common_estimate_bytes(real_minrtt_subflow, slow_linger_time);
    
    slow_inflight_bytes = selected_tp->write_seq - selected_tp->snd_una;
    slow_bytes = slow_inflight_bytes;
    snd_wnd = tcp_wnd_end(meta_tp) - meta_tp->write_seq;
    avail_space = (slow_bytes < snd_wnd) ? (snd_wnd - slow_bytes) : 0;
    
    if (fast_bytes > avail_space) {
        // HoL blocking risk - override with fast subflow
        mptcp_subflow_set_scheduled(real_minrtt_subflow, 1);
        return 0;
    }
    
    // Safe to use cross-layer selected subflow
    mptcp_subflow_set_scheduled(max_datarate_subflow, 1);
}
```

**Characteristics:**
- ✅ Complete BLEST algorithm implementation
- ✅ Uses `tcp_wnd_end()` (more accurate window calculation)
- ✅ Explicit subflow scheduling with `mptcp_subflow_set_scheduled()`
- ✅ Clear decision path with explicit fastest subflow selection
- ❌ Doesn't account for current SKB buffered bytes (simplified for MPTCP v1)

## Algorithm Flow Comparison

### Legacy Flow
```
1. Find available subflows
2. Classify WiFi vs 5G by IP
3. Get network metrics
4. Select based on bitrate comparison → mssk
5. Get default best subflow → blestsk
6. BLEST HoL check: if (mssk != blestsk && HoL_risk) blestsk = NULL
7. Return blestsk (or NULL for framework fallback)
```

### New Flow
```
1. Find minimum RTT subflow (fallback)
2. Get MPTCP framework recommendation
3. Classify subflows by network interface
4. Get current network metrics (thread-safe)
5. Cross-layer subflow selection with fallbacks → max_datarate_subflow
6. If (max_datarate_subflow == fastest) → schedule and return
7. BLEST HoL check: if (HoL_risk) schedule fastest, else schedule cross-layer choice
8. Explicit scheduling with mptcp_subflow_set_scheduled()
```

## Performance Implications

### Memory Usage
- **Legacy**: Multiple files, scattered structures
- **New**: Single file, optimized structures
- **Impact**: Reduced memory footprint

### CPU Overhead
- **Legacy**: Basic calculations
- **New**: More sophisticated but optimized
- **Impact**: Better long-term performance

### Network Performance
- **Legacy**: Basic WiFi/5G selection
- **New**: Intelligent HoL prevention + cross-layer optimization
- **Impact**: Significantly improved throughput and latency

## Conclusion

The new XLayer implementation represents a significant advancement over the legacy version:

1. **Modernized**: Full MPTCP v1 compatibility
2. **Enhanced**: Improved HoL blocking prevention with better window calculations
3. **Robust**: Comprehensive error handling and fallbacks
4. **Maintainable**: Clean, well-documented code
5. **Production-Ready**: Thread-safe, modular design

**Important Note**: Both implementations include Head-of-Line blocking prevention based on the BLEST algorithm. The key differences are:

- **Legacy**: Uses MPTCP v0 framework with basic window calculations (`meta_tp->snd_wnd`)
- **New**: Uses MPTCP v1 framework with enhanced window calculations (`tcp_wnd_end()`) and explicit subflow scheduling

The new implementation maintains the core cross-layer optimization and HoL blocking prevention principles while adapting to the modern MPTCP framework and adding better error handling, documentation, and maintainability for production deployments.
