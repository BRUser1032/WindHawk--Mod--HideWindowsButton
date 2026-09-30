# Hide Start Button (hardened) — mod do Windhawk

Esconde o botão Iniciar da barra de tarefas do **Windows 11**. O menu Iniciar continua abrindo com a tecla Win, Ctrl+Esc ou gestos de toque. Desativar o mod traz o botão de volta.

Mod apresentado no canal **@BitRizeBR** (YouTube), desenvolvido com ajuda do Claude (IA).

> **Windows 10 não é suportado.** O mod depende da barra de tarefas XAML do Windows 11.

---

## Instalação rápido (menos de 5 minutos)

1. Instale o [Windhawk](https://windhawk.net).
2. No Windhawk, clique em **Create a new mod**.
3. Apague o conteúdo do editor e cole o conteúdo de `hide-start-button-hardened.wh.cpp`.
4. Clique em **Compile** e depois em **Enable mod**.
5. Confira se o botão Iniciar sumiu e se os ícones ocuparam o espaço dele.

Para voltar ao normal: desative o mod (o botão reaparece). Se o Explorer se comportar de forma estranha, desative o mod e reinicie o Explorer.

---

## Status de testes

| Windows | Build | Versão do mod | Resultado |
|---|---|---|---|
| 11 25H2 | 26200.9550 | 1.0.1 | Funciona, sem erros no log |
| 11 25H2 | 26200.9550 | 1.1.0 | **A confirmar** (atualize esta linha após o teste) |
| Outras builds | — | — | **Não testado** |

**Não há garantia de que continue funcionando após uma atualização do Windows.** Veja "Limitações".

---

## Configurações

- **Allow untested Windows builds** (desligado por padrão): a partir da 1.1.0 o mod só carrega nas builds testadas. Ligue esta opção para tentar em outra build e **olhe a aba Log**.

Para ver as mensagens do mod, ative o log de depuração do mod na aba *Advanced* do Windhawk.

---

## O que este fork muda em relação ao original

Base: "Hide Start Button" de ptrkhh, que por sua vez é baseado em "Start button always on the left" (taskbar-start-button-position) de m417z.

- Só carrega em builds testadas (a menos que o usuário autorize).
- Valida que a função interna do XAML que ele intercepta pertence a um módulo XAML. Em caso de dúvida, entra em **modo degradado**: o botão some, mas o espaço vazio pode continuar, e o mod não instala o hook arriscado.
- Confere se a memória interna é legível antes de lê-la.
- Registra no log a build do Windows, a origem do offset interno e o módulo/endereço da função interceptada.
- Proteções contra exceções C++ em volta de chamadas XAML e callbacks.
- `SendMessageTimeout` (2 s) no lugar de `SendMessage`.

---

## Limitações (leia antes de instalar)

- O mod usa **partes internas e não documentadas do Windows** (símbolos de `taskbar.dll` e um índice fixo de tabela virtual do XAML). Uma atualização do Windows pode mudá-las.
- O mod **não** verifica que o índice 92 é realmente a função esperada; só verifica que o endereço está num módulo XAML.
- `try/catch` **não captura falhas de memória** (access violation). As checagens de leitura reduzem o risco, mas não o eliminam.
- Se os símbolos não forem encontrados, o mod não carrega (falha segura). Se só o índice mudar, o Explorer pode se comportar de forma anômala.
- Testado por uma pessoa, em uma máquina.

---

## Como reportar uma build nova ou um problema

Abra um issue e inclua:

1. Versão e build do Windows (`winver`).
2. Versão do mod.
3. As linhas do Log do Windhawk (com o log de depuração ligado), principalmente `Windows build`, `TaskbarHost offset`, `Arrange candidate` e qualquer `degraded mode`.
4. O que você esperava e o que aconteceu.

Com essas linhas dá para adicionar novas builds à lista de testadas com segurança.

---

## Créditos e licença

- **ptrkhh** — mod original "Hide Start Button".
- **m417z** — mod "Start button always on the left" (base do original) e o próprio Windhawk.
- **BRUser1032 / BitRize** — este fork.
- **Claude (Anthropic)** — assistência na revisão e no reforço do código.

Este código é **derivado** dos mods acima. Reescrever partes dele, inclusive com ajuda de IA, não remove os direitos dos autores originais.

**Licença: pendente de verificação.** Este repositório ainda não declara licença. No repositório oficial de mods do Windhawk, mods sem a tag `@license` são enviados sob a licença MIT; se o original estiver nessa situação, é preciso manter o aviso de copyright e a licença do original junto deste código. Até a verificação ser concluída, trate o uso como "sem licença declarada".

Este projeto não é afiliado ao Windhawk nem a seus autores.

---

## English summary

Windhawk mod that hides the Start button on Windows 11 (not Windows 10). Hardened fork of ptrkhh's "Hide Start Button" (itself based on m417z's "Start button always on the left"). Loads only on tested builds (currently Windows 11 25H2, build 26200) unless "Allow untested Windows builds" is enabled. It relies on undocumented Windows internals and may break after a Windows update. AI-assisted; tested on a single machine. License pending verification.
