# Contribuindo

O foco é evidência reproduzível e mudanças que não prejudiquem a coleta comprovada.

- Preserve a independência GUI/monitor/coletor, a imutabilidade dos ZIPs e a separação do RETURN.
- Execute `PYTHONPATH=tools/drive-gpu-lab/android python3 -m pytest tools/drive-gpu-lab/tests -q`, `go vet` e build Win64.
- Não versione ZIPs pessoais, logs, dumps, binários compilados, tokens ou dados de dispositivos da comunidade.
- Não confunda chamadas citadas pelo Wine com operações internas concluídas pelo PanVK.
- Sempre informe se uma correção foi testada apenas offline ou também no Wine real.

**Problema conhecido:** exceções GUI `0xC0000005` ainda precisam de backtrace nativo e reprodução causal.
