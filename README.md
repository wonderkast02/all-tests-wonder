# 🧪 All Tests Wonder

Ferramentas de **teste, telemetria, diagnóstico e qualificação** para gráficos e runtimes.

Projeto independente para validar ferramentas, builds e ambientes com evidências reproduzíveis.

---

## 🚀 Drive GPU Lab

**Versão atual:** `0.1.0-dev.1`  
**Status:** 🟢 pré-versão de desenvolvimento qualificada

O **Drive GPU Lab** coleta e organiza informações de testes no **Winlator / Wine / Vulkan / PanVK**, ajudando a identificar com precisão onde um problema começou e em qual ambiente ele ocorreu.

### 🧩 Componentes

- 🖥️ **`G720Probe.exe`** — supervisor, HUD, sessões e telemetria no Windows/Wine/Winlator
- 🎮 **`G720VkLayer.dll`** — camada Vulkan para present, submits, memória e eventos de falha
- 📱 **`g720-hostd`** — telemetria do host Android/Linux

### 📊 Coleta

- FPS e frametime
- CPU, RAM, GPU, frequência e temperatura
- Vulkan, DXVK, VKD3D-Proton, Wine, Box64 e FEX
- Mesa / PanVK e módulos gráficos carregados
- hashes SHA-256 dos binários relevantes
- eventos, marcadores de bug e contexto da sessão
- logs separados por jogo e por execução

---

## ✅ Qualificação

| Etapa | Estado |
|---|:---:|
| 🧹 Qualidade do código-fonte | ✅ |
| 🐧 Linux x86_64 | ✅ |
| 📱 Android arm64-v8a | ✅ |
| 🪟 Windows x64 | ✅ |
| 🪟 Windows x86 | ✅ |
| 🔐 SHA-256 dos pacotes | ✅ |
| 🧩 Arquiteturas ELF / PE | ✅ |
| 🎮 ABI / exports da camada Vulkan | ✅ |

---

## 📦 Download

### **[⬇️ Drive GPU Lab 0.1.0-dev.1](https://github.com/wonderkast02/all-tests-wonder/releases/tag/drive-gpu-lab-0.1.0-dev.1)**

Escolha o pacote correspondente ao ambiente que será testado.

---

## 🗂️ Organização

Cada jogo mantém uma pasta própria. Cada execução cria apenas uma nova sessão.

```text
DriveGpuLab/
└── games/
    └── nome-do-jogo/
        ├── game.json
        ├── history.jsonl
        ├── latest.txt
        └── sessions/
            └── <sessão>/
```

Assim, resultados antigos não são misturados com testes novos.

---

## 🎯 Princípios

- evidência antes de suposição;
- dados indisponíveis permanecem desconhecidos;
- cada resultado pertence a uma sessão e ambiente exatos;
- builds e artefatos devem ser verificáveis;
- desempenho só é comparado com condições controladas.

---

## ⚠️ Estado do projeto

`Drive GPU Lab 0.1.0-dev.1` é uma ferramenta de **instrumentação para desenvolvimento**.

Não representa certificação oficial de conformidade Vulkan e não garante compatibilidade universal com jogos.

---

## 📄 Licenciamento

O **All Tests Wonder** utiliza licenciamento por componente.

O Drive GPU Lab possui licença própria e identificadores SPDX em `tools/drive-gpu-lab/`.
