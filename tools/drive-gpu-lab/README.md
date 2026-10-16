# GPU Lab — Código-fonte R5.7

Esta é a implementação atual do GPU Lab. O protótipo anterior em C (G720Probe, G720VkLayer e hostd) permanece recuperável **pelo histórico Git e pela release histórica**, mas não faz parte desta candidata Go/Python.

```text
windows/   Go 1.23 — GPU-Lab.exe, supervisor, GUI, telemetria e controle
android/   Python 3 — coleta e análise de evidências
tests/    pytest — 70 verificações automatizadas
 docs/     Variáveis e diagnóstico PanVK
```

## Compilar e testar

```bash
python3 -m pip install pytest==9.0.2
PYTHONPATH=android python3 -m pytest tests -q
cd windows
GOOS=windows GOARCH=amd64 CGO_ENABLED=0 go build -trimpath -ldflags='-s -w' -o GPU-Lab.exe .
GOOS=windows GOARCH=amd64 CGO_ENABLED=0 go vet ./...
```

O instalador autocontido e os binários gerados são **assets de release**, não arquivos-fonte versionados no Git. O release inclui hashes SHA256. Testes Linux e compilação Windows x64 não substituem teste físico em Wine/Bannerlator.

**Sem DLL Vulkan instrumentada nesta candidata.** A análise de Vulkan/PanVK é feita nos logs existentes, com atribuição e limites explicitados. Não declarar FPS, CSF/fences ou identidade de biblioteca como demonstrados se as fontes não foram efetivamente capturadas.

[Visão geral](../../README.md) · [Histórico](CHANGELOG.md) · [Variáveis](docs/LOG-VARIABLES.md) · [PanVK](docs/PANVK-DIAGNOSTICS.md)
