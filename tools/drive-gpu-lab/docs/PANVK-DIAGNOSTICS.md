# GPU Lab — Variáveis para evidências PanVK/Mesa

Estas variáveis devem chegar **ao processo que carrega o PanVK**, dentro do container/runtime apropriado. Defini-las apenas no Termux do coletor **não** altera retroativamente o jogo. O GPU Lab nunca injeta layers, modifica a biblioteca Vulkan ou altera variáveis do jogo automaticamente.

## Perfil padrão (isolado e leve)

```ini
DXVK_LOG_LEVEL=info
PANVK_DEBUG=startup
```

## Perfil profundo (um problema por rodada)

```ini
DXVK_LOG_LEVEL=debug
DXVK_DEBUG=hang
PANVK_DEBUG=startup,sync
```

## Investigação de fluxo de comandos (somente build que suporte estas flags)

```ini
PANVK_DEBUG=startup,trace,cs
```

`PANVK_DEBUG=trace`, `sync`, `cs` e `startup` aparecem na tabela de opções do **Mesa 26.2**; ports Android/kbase podem ter configuração diferente. Consulte o código da build em execução. Log de startup confirma apenas a ativação reportada — não garante que o arquivo chegou ao ZIP nem que há eventos internos de submissão.

## Destinos e outras fontes (configurar de acordo com o ambiente)

```ini
MESA_LOG_FILE=<CAMINHO_NATIVE_GRAVAVEL>/mesa.log
DXVK_LOG_PATH=<PASTA_GRAVAVEL_DO_PROCESSO_DXVK>
WINEDEBUG=+err,+warn,+seh,+vulkan
GPU_LAB_EXTRA_LOG_ROOT=<PASTA_DE_LOGS_ACESSIVEL_AO_TERMUX>
GPU_LAB_DRIVER_PATH=<SO_CANDIDATA_NO_TERMUX>
```

A última variável deve ser especificada **sem espaços** no nome do caminho ou com aspas adequadas. O hash de um arquivo candidato não comprova qual biblioteca o jogo carregou. `MESA_LOG_FILE` não produz logs se a biblioteca instalada ignorar a variável ou se o destino não for gravável. `WINEDEBUG` é uma fonte de chamadas ao Wine, **não um trace do driver**.

## Fontes e limitações

- Mesa/PanVK 26.2 — `panvk_instance.c`: https://fossies.org/linux/misc/mesa-26.2.0.tar.xz/mesa-26.2.0/src/panfrost/vulkan/panvk_instance.c
- Mesa, variáveis: https://docs.mesa3d.org/envvars.html
- DXVK, debug: https://github.com/doitsujin/dxvk/blob/master/README.md

**Não ativar indiscriminadamente** trace/cs, layers Vulkan ou `MESA_VK_ABORT_ON_DEVICE_LOSS`: podem alterar timing, gerar muitos dados ou abortar processos. `PANVK-EVIDENCE.json` distingue mensagens existentes de instrumentação e não afirma que o driver interno foi rastreado sem evidências reais.
