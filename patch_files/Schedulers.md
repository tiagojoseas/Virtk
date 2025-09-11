# MPTCP Schedulers - Architecture and Relationships

Este documento explica a arquitetura e relacionamentos entre os diferentes schedulers MPTCP implementados neste projeto.

## Overview da Arquitetura

O projeto implementa três schedulers MPTCP que compartilham funcionalidades comuns através de uma biblioteca compartilhada (`common_lib.h`), promovendo reutilização de código e consistência.

```
┌─────────────────────────────────────────────────────────────┐
│                    common_lib.h                             │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────── │
│  │ Shared Functions│  │ BLEST Functions │  │ Parameters     │ │
│  │ - RTT selection │  │ - Lambda update │  │ - lambda       │ │
│  │ - Availability  │  │ - Byte estimate │  │ - max_lambda   │ │
│  │ - Fallback      │  │ - Linger time   │  │ - min_lambda   │ │
│  └─────────────────┘  └─────────────────┘  └─────────────── │
└─────────────────────────────────────────────────────────────┘
           │                    │                    │
           ▼                    ▼                    ▼
    ┌──────────┐         ┌──────────┐         ┌──────────┐
    │ minrtt.c │         │ blest.c  │         │xlayer.c │
    └──────────┘         └──────────┘         └──────────┘
```

## Schedulers Implementados

### 1. MinRTT Scheduler (`minrtt.c`)

**Propósito**: Scheduler básico que seleciona sempre o subflow com menor RTT.

**Características**:
- Algoritmo simples e eficiente
- Prioriza subflows ativos sobre backup
- Usa funções compartilhadas para seleção RTT

**Dependências**:
- `__mptcp_sched_minrtt_get_subflow()` - Seleção do melhor subflow
- `mptcp_select_fallback_subflow()` - Fallback quando não há subflows disponíveis

```c
// Algoritmo simplificado
best_subflow = __mptcp_sched_minrtt_get_subflow(msk, data);
if (best_subflow) {
    schedule(best_subflow);
} else {
    fallback_subflow = mptcp_select_fallback_subflow(msk);
    schedule(fallback_subflow);
}
```

### 2. BLEST Scheduler (`blest.c`)

**Propósito**: Scheduler avançado com prevenção de Head-of-Line (HoL) blocking.

**Características**:
- Baseado no algoritmo BLEST original
- Prevenção dinâmica de HoL blocking
- Ajuste automático do parâmetro lambda
- Estatísticas de desempenho

**Dependências**:
- `find_min_rtt_subflow()` - Encontra subflow mais rápido
- `__mptcp_sched_minrtt_get_subflow()` - Seleciona melhor subflow para envio
- `common_update_lambda()` - Atualiza parâmetro lambda dinamicamente

**Algoritmo**:
```c
// 1. Encontra subflows candidatos
fastest_subflow = find_min_rtt_subflow(msk);
best_subflow = __mptcp_sched_minrtt_get_subflow(msk, data);

// 2. Verifica risco de HoL blocking
if (hol_blocking_risk(best_subflow, fastest_subflow)) {
    schedule(fastest_subflow);  // Usa subflow rápido
    hol_prevented++;
} else {
    schedule(best_subflow);     // Seguro usar subflow lento
}
```

### 3. XLayer Scheduler (`xlayer.c`)

**Propósito**: Scheduler cross-layer que otimiza baseado em informações de rede (WiFi/5G).

**Características**:
- Otimização cross-layer com métricas WiFi/5G
- Thread background para coleta de métricas
- Interface proc para configuração externa
- Prevenção HoL baseada no BLEST
- Classificação inteligente de subflows

**Dependências**:
- `find_min_rtt_subflow()` - Subflow de fallback
- `mptcp_subflow_is_available()` - Verificação de disponibilidade
- `common_update_lambda()` - Prevenção HoL
- Funções específicas: `get_wifi_info_by_name()`, `is_ip_of_wifi()`

**Algoritmo**:
```c
// 1. Classificação de subflows
for_each_subflow() {
    if (is_ip_of_wifi(ip)) {
        wifi_subflow = subflow;
    } else {
        cellular_subflow = subflow;
    }
}

// 2. Seleção cross-layer
if (wifi_bitrate > cellular_bitrate) {
    best_subflow = wifi_subflow;
} else {
    best_subflow = cellular_subflow;
}

// 3. Prevenção HoL (baseada no BLEST)
if (hol_blocking_risk()) {
    schedule(fastest_subflow);
} else {
    schedule(best_subflow);
}
```

## Biblioteca Compartilhada (`common_lib.h`)

### Funções Compartilhadas

#### Seleção de Subflows
```c
// Seleção básica por RTT
mptcp_subflow_is_available()       // Verifica disponibilidade
mptcp_subflow_get_rtt()            // Obtém RTT efetivo
get_minrtt_sock()                  // Encontra subflow com menor RTT
__mptcp_sched_minrtt_get_subflow() // Seleção completa com fallback
find_min_rtt_subflow()             // Versão BLEST-aware
```

#### Funções BLEST
```c
// Estimativa e prevenção HoL
common_estimate_bytes()            // Estima bytes a serem enviados
common_estimate_linger_time()      // Estima tempo de permanência
common_update_lambda()             // Atualiza parâmetro lambda
blest_subflow_is_available()       // Disponibilidade com bounds RTT
```

#### Parâmetros Compartilhados
```c
lambda          // Fator de escala principal
max_lambda      // Valor máximo do lambda
min_lambda      // Valor mínimo do lambda
dyn_lambda_good // Decremento em casos positivos
dyn_lambda_bad  // Incremento em casos negativos
```

## Relacionamentos e Hierarquia

### Hierarquia de Complexidade
```
MinRTT (Básico) → BLEST (HoL Prevention) → XLayer (Cross-Layer + HoL)
```

### Dependências Funcionais
```
┌─────────────┐
│   MinRTT    │ ← Base: Seleção RTT simples
└─────────────┘
       │
       ▼
┌─────────────┐
│   BLEST     │ ← Adiciona: Prevenção HoL + Lambda dinâmico
└─────────────┘
       │
       ▼
┌─────────────┐
│   XLayer    │ ← Adiciona: Cross-layer + Métricas em tempo real
└─────────────┘
```

### Compartilhamento de Código

| Função | MinRTT | BLEST | XLayer |
|--------|--------|-------|--------|
| `mptcp_subflow_is_available()` | ✓ | ✓ | ✓ |
| `__mptcp_sched_minrtt_get_subflow()` | ✓ | ✓ | - |
| `find_min_rtt_subflow()` | - | ✓ | ✓ |
| `common_update_lambda()` | - | ✓ | ✓ |
| Parâmetros lambda | - | ✓ | ✓ |

## Configuração e Uso

### Ativação dos Schedulers
```bash
# MinRTT
echo minrtt > /proc/sys/net/mptcp/scheduler

# BLEST
echo blest > /proc/sys/net/mptcp/scheduler

# XLayer
echo xlayer > /proc/sys/net/mptcp/scheduler
```

### Configuração de Parâmetros
```bash
# Parâmetros BLEST/XLayer
echo 15 > /sys/module/blest/parameters/lambda      # BLEST
echo 15 > /sys/module/xlayer/parameters/lambda     # XLayer (usa compartilhado)

# Configuração 5G no XLayer
echo "150000000" > /proc/xlayer_5g_proc
```

### Scripts de Teste
```bash
# Teste com diferentes schedulers
./iperf.sh -c SERVER_IP minrtt
./iperf.sh -c SERVER_IP blest
./iperf.sh -c SERVER_IP xlayer
```

## Estatísticas e Monitoramento

### BLEST e XLayer
- `total_decisions`: Total de decisões de scheduling
- `hol_prevented`: Casos onde HoL blocking foi prevenido
- Logs detalhados sobre seleção de subflows

### XLayer Específico
- Métricas WiFi/5G em tempo real
- Thread background para coleta contínua
- Interface proc para configuração externa

## Vantagens da Arquitetura Compartilhada

1. **Reutilização de Código**: Evita duplicação entre schedulers
2. **Consistência**: Todos usam a mesma lógica base
3. **Manutenibilidade**: Correções e melhorias em um local
4. **Modularidade**: Cada scheduler foca em sua especialidade
5. **Performance**: Funções otimizadas compartilhadas
6. **Testabilidade**: Testes comuns para funcionalidades base

## Associações e Interoperabilidade entre Schedulers

### 1. Associação por Herança de Funcionalidades

Os três schedulers seguem uma progressão evolutiva de funcionalidades:

#### MinRTT → BLEST (Associação por Extensão)
```c
// MinRTT implementa a base
minrtt_logic() {
    subflow = get_best_rtt_subflow();
    schedule(subflow);
}

// BLEST ESTENDE MinRTT adicionando HoL prevention  
blest_logic() {
    fast_subflow = get_best_rtt_subflow();        // ← Reutiliza MinRTT
    best_subflow = get_optimal_subflow();
    
    if (hol_blocking_detected(fast, best)) {      // ← Adiciona HoL
        schedule(fast_subflow);
    } else {
        schedule(best_subflow);
    }
}
```

#### BLEST → XLayer (Associação por Composição)
```c
// XLayer COMPÕE BLEST + Cross-layer optimization
xlayer_logic() {
    fast_subflow = get_best_rtt_subflow();        // ← De MinRTT
    
    // Cross-layer decision (novo)
    wifi_subflow = classify_wifi_subflow();
    cellular_subflow = classify_cellular_subflow();
    best_subflow = select_by_bitrate(wifi, cellular);
    
    // HoL prevention (do BLEST)
    if (hol_blocking_detected(fast, best)) {      // ← De BLEST
        schedule(fast_subflow);
    } else {
        schedule(best_subflow);
    }
}
```

### 2. Associação por Compartilhamento de Estado

Os schedulers compartilham estado através de estruturas globais:

#### Estrutura de Estado Compartilhado
```c
// common_lib.h - Estado global compartilhado
struct blest_conn_data common_global_data = {
    .lambda_1000 = 1200,           // ← Usado por BLEST e XLayer
    .min_srtt_us = U32_MAX,        // ← Atualizado por todos
    .max_srtt_us = 0,              // ← Atualizado por todos
};

// Cada scheduler contribui para o estado global
find_min_rtt_subflow() {
    // MinRTT, BLEST e XLayer atualizam bounds RTT
    common_global_data.min_srtt_us = min(current, tp->srtt_us);
    common_global_data.max_srtt_us = max(current, tp->srtt_us);
}
```

#### Fluxo de Estado Entre Schedulers
```
┌─────────────┐    ┌─────────────┐    ┌─────────────┐
│   MinRTT    │    │   BLEST     │    │   XLayer    │
│             │    │             │    │             │
│ Atualiza:   │───▶│ Usa:        │───▶│ Usa:        │
│ RTT bounds  │    │ RTT bounds  │    │ RTT bounds  │
│             │    │ + lambda    │    │ + lambda    │
│             │    │ Atualiza:   │    │ + metrics   │
│             │    │ lambda      │    │             │
└─────────────┘    └─────────────┘    └─────────────┘
```

### 3. Associação por Interfaces Comuns

Todos implementam a mesma interface MPTCP:

```c
// Interface padrão MPTCP - implementada por todos
struct mptcp_sched_ops {
    void (*init)(struct mptcp_sock *msk);
    void (*release)(struct mptcp_sock *msk);
    int (*get_subflow)(struct mptcp_sock *msk, struct mptcp_sched_data *data);
    char *name;
    struct module *owner;
};

// MinRTT - Implementação mínima
static struct mptcp_sched_ops mptcp_sched_minrtt = {
    .init = basic_init,
    .get_subflow = basic_rtt_selection,
    .name = "minrtt",
};

// BLEST - Estende com estatísticas
static struct mptcp_sched_ops mptcp_sched_blest = {
    .init = blest_init_with_stats,           // ← Adiciona estatísticas
    .get_subflow = blest_with_hol_prevention, // ← Adiciona HoL
    .release = blest_release_with_stats,     // ← Mostra estatísticas  
    .name = "blest",
};

// XLayer - Estende com threads e proc
static struct mptcp_sched_ops mptcp_sched_xlayer = {
    .init = xlayer_init_with_threads,        // ← Adiciona threads
    .get_subflow = xlayer_cross_layer,       // ← Cross-layer + HoL
    .release = xlayer_cleanup_threads,       // ← Cleanup completo
    .name = "xlayer",
};
```

### 4. Associação por Padrões de Decisão

#### Matriz de Decisão Comparativa
```
Situação                    │ MinRTT │ BLEST  │ XLayer
──────────────────────────────────────────────────────
2 subflows, RTT similares   │   A    │   A    │ WiFi/5G
RTT muito diferentes        │   A    │   A    │    A
HoL blocking detectado      │   A    │   B    │    B  
WiFi > 5G bitrate          │   A    │   ?    │  WiFi
5G > WiFi bitrate          │   A    │   ?    │   5G
Sem métricas cross-layer    │   A    │   A    │    A

Legenda: A = subflow RTT mínimo, B = subflow mais rápido
```

#### Fluxo de Decisão Comparativo
```
Todos os Schedulers seguem este padrão base:
┌─────────────────────────────────────────────────────────┐
│ 1. Enumerate subflows available                         │
│ 2. Check basic availability (shared function)          │
│ 3. Apply scheduler-specific selection logic             │
│ 4. Verify if selection is safe (HoL check if capable)  │  
│ 5. Schedule selected subflow                            │
└─────────────────────────────────────────────────────────┘

MinRTT: Skip steps 4 (no HoL check)
BLEST:  Full process with HoL prevention
XLayer: Full process + cross-layer metrics in step 3
```

### 5. Associação por Debugging e Telemetria

#### Sistema de Logging Comum
```c
// Todos usam o mesmo padrão de debug
pr_debug("%s: selected subflow (reason)\n", scheduler_name);
pr_debug("%s: HoL prevention triggered\n", scheduler_name);  // BLEST, XLayer
// pr_info("%s: statistics - decisions: %u, hol_prevented: %u\n", 
        scheduler_name, decisions, prevented);               // BLEST, XLayer
```

#### Telemetria Incremental
```
MinRTT:  Logging básico de seleção
         ↓
BLEST:   + Estatísticas HoL + Lambda tracking
         ↓  
XLayer:  + Métricas cross-layer + Thread status
```

### 6. Associação por Parâmetros de Configuração

#### Hierarquia de Parâmetros
```bash
# Parâmetros Globais (common_lib.h)
/sys/module/*/parameters/lambda          # Usado por BLEST e XLayer
/sys/module/*/parameters/max_lambda      # Usado por BLEST e XLayer
/sys/module/*/parameters/min_lambda      # Usado por BLEST e XLayer

# Parâmetros Específicos XLayer
/proc/xlayer_5g_proc                     # Apenas XLayer

# MinRTT não tem parâmetros próprios (usa apenas shared functions)
```

### 7. Associação por Compatibilidade de Transição

Os schedulers são projetados para permitir migração sem impacto:

```bash
# Transição sem reinicialização
echo minrtt > /proc/sys/net/mptcp/scheduler   # Base simples
echo blest > /proc/sys/net/mptcp/scheduler    # Adiciona HoL prevention
echo xlayer > /proc/sys/net/mptcp/scheduler   # Adiciona cross-layer

# Estado compartilhado é preservado entre transitions
# Lambda values, RTT bounds, etc. são mantidos
```

### 8. Padrão de Associação: Template Method

Os schedulers seguem o padrão Template Method:

```c
// Template comum (implícito em todos)
int scheduler_template(msk, data) {
    // 1. Base RTT analysis (todos implementam)
    base_subflow = analyze_rtt_subflows(msk);
    
    // 2. Scheduler-specific optimization (template method)
    optimal_subflow = apply_optimization_strategy(msk, data);
    
    // 3. Safety check (BLEST e XLayer implementam)
    if (safety_check_enabled() && hol_risk_detected()) {
        return schedule(base_subflow);
    }
    
    // 4. Schedule optimal choice
    return schedule(optimal_subflow);
}

// MinRTT: apply_optimization_strategy() retorna base_subflow
// BLEST:  apply_optimization_strategy() considera congestion
// XLayer: apply_optimization_strategy() considera bitrates
```

Esta associação em camadas permite que cada scheduler construa sobre os anteriores, mantendo compatibilidade e reutilizando funcionalidades testadas.