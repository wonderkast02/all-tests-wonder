<div align="center">

# 🧪 GPU Lab

### Diagnóstico reproduzível para Winlator, Wine e PanVK

**`Jogo → DXVK / VKD3D → Wine / Vulkan → Mesa / PanVK`**

![Status](https://img.shields.io/badge/STATUS-PRÉ--VERSÃO_EXPERIMENTAL-F59E0B?style=for-the-badge)
![Versão](https://img.shields.io/badge/GPU_LAB-R5.7-2563EB?style=for-the-badge)
![Windows](https://img.shields.io/badge/MONITOR-WINDOWS_X64-2563EB?style=for-the-badge)
![Android](https://img.shields.io/badge/COLETA-TERMUX-3DDC84?style=for-the-badge)

**Logs reais · Sessões isoladas · Integridade verificável**

</div>

---

> [!WARNING]
> **R5.7 é uma candidata experimental.** A coleta foi testada no Bannerlator, porém a GUI apresentou `0xC0000005` e recuperação automática. Não é uma release estável nem substitui instrumentação interna do driver.

## Estado validado

| Recurso | Situação |
| --- | :---: |
| Detecção automática do jogo e PID | ✅ |
| Acompanhamento de processos, CPU e RAM | ✅ |
| Coleta e organização dos logs DXVK/Wine disponíveis | ✅ |
| Arquivamento por jogo: ZIP, CRC e SHA256 | ✅ |
| Encerramento após o jogo e RETURN independente | ✅ |
| Relatório `DRIVER-EVIDENCE.json` | ✅ |
| Relatório `PANVK-EVIDENCE.json` com leitura completa dos logs selecionados | ✅ |
| Reconhecimento de mensagens `SYNC-A3` | ✅ |
| Recuperação da GUI após falha | 🧪 |
| GUI livre de exceções nativas | ❌ |
| Trace interno PanVK de submits/fences/CSF | ❌ Não comprovado |
| Biblioteca PanVK realmente carregada pelo jogo | ❌ Não comprovada |
| FPS/frametime por camada Vulkan | ❌ Não instrumentado nesta linha |

✅ Validado no escopo indicado · 🧪 Experimental · ❌ Não implementado ou não comprovado.

## Download

**[⬇️ GPU Lab R5.7 — pré-versão de desenvolvimento](https://github.com/wonderkast02/all-tests-wonder/releases/tag/gpu-lab-r5.7-dev)**

Baixe `GPU-Lab-R5_7-PanVK-FullScan-GUISafety.sh` nos assets da release, verifique `SHA256SUMS.txt` e execute com Termux. O SH inclui o executável Windows x64 e o coletor Android; **não é APK**.

## Arquitetura

```text
Bannerlator / Winlator                Termux / Android
┌────────────────────────┐            ┌───────────────────────────┐
│ GPU-Lab.exe            │            │ gpu_lab_collector.py      │
│ ├─ GUI                 │            │ ├─ diagnóstico Android   │
│ ├─ supervisor          │  arquivos  │ ├─ evidência DXVK/Wine    │
│ └─ monitor do jogo     ├───────────►│ ├─ análise Mesa / PanVK   │
└────────────────────────┘            │ └─ ZIP imutável por sessão│
                                      └───────────────────────────┘
```

O GPU Lab **não injeta código no jogo**, não modifica o driver e não precisa interromper o processo monitorado. Coleta arquivos de log disponíveis e telemetria visível ao runtime; fontes bloqueadas pelo Android permanecem `NOT_PROVEN` ou indisponíveis.

## Arquivos gerados

```text
Download/
├── GPU-Lab/
│   └── Logs/
│       ├── speed.exe-<sessão>/
│       └── speed.exe-<sessão>.zip
└── Log Termux/
    └── GPU-Lab-RETURN-<execução>.zip
```

O ZIP do jogo guarda logs brutos, telemetria, relatórios `DRIVER-EVIDENCE.json` e `PANVK-EVIDENCE.json`, metadados e verificações de integridade. O RETURN contém apenas os diagnósticos da execução e do instalador; não duplica o ZIP completo do jogo.

## Funções

| Bloco | O que faz |
| --- | --- |
| Monitor | Reconhece executáveis de jogo, associa PID, detecta saída, mantém sessão independente da GUI |
| Interface | Estados `AGUARDANDO` / `GRAVANDO` / `FINALIZADO`, indicadores disponíveis e recuperação após queda |
| Logs externos | Procura arquivos recentes de DXVK, VKD3D, Wine, Box64, FEX, Vulkan, Mesa e PanVK **quando existirem e forem acessíveis** |
| Evidências | Preserva fontes brutas; sumariza mensagens Vulkan, erros, sync, VUIDs e origem dos dados sem inventar sucesso de chamadas |
| PanVK | Varredura por streaming e marcadores `SYNC-A3`; aponta limites e Wine compartilhado |
| Android | Snapshot de ambiente, tentativa de `logcat`/kernel e GPU sysfs quando permitido; ausência explicitada |
| Integridade | Pastas por sessão, arquivos ZIP imutáveis, SHA256, CRC, prevenção de colisões e duplicatas |
| Finalização | Detecta encerramento do jogo e gera o RETURN sem obrigar o fechamento do processo-alvo |

## Variáveis de diagnóstico

**[Lista completa de variáveis](tools/drive-gpu-lab/docs/LOG-VARIABLES.md)** · **[PanVK/Mesa](tools/drive-gpu-lab/docs/PANVK-DIAGNOSTICS.md)**

Perfil leve: `DXVK_LOG_LEVEL=info`. Para investigar erros DXVK: `DXVK_LOG_LEVEL=debug`. Para PanVK: `PANVK_DEBUG=startup,sync` **somente se a build efetivamente suportar as opções**. Defina variáveis no processo/container que executa o componente, não apenas no coletor Termux.

## Validação e limitações

- **Hardware real:** Bannerlator, `speed.exe` (Need For Speed Most Wanted Black Edition), sessões ZIP e RETURN íntegros. Última execução R5.7: **45 menções `SYNC-A3`**, extraídas de Wine **compartilhado**, sem atribuição exclusiva ao PID.
- **Código:** 70 testes de regressão aprovados; Go Windows x64 compilado e submetido a `go vet`. Isso **não** comprova ausência de falhas sob Wine.
- **Problema conhecido:** `0xC0000005` na GUI; supervisor recuperou a coleta, mas causa exata ainda não foi isolada.
- **Fontes não garantidas:** logcat sem permissão, kernel Mali/CSF, FPS real e biblioteca nativa carregada. O programa informa lacunas em vez de fabricá-las.
- **Privacidade:** logs Wine podem conter caminhos e informações pessoais. Revise antes de publicar.

[**Notas da versão**](tools/drive-gpu-lab/CHANGELOG.md) · [**Código-fonte**](tools/drive-gpu-lab/) · [**Como contribuir**](CONTRIBUTING.md) · [**Segurança**](SECURITY.md) · [**Licença**](LICENSING.md)

---

<div align="center">

**GPU Lab · Projeto All Tests Wonder**

**Evidência reproduzível antes de suposições.**

</div>
