# WindHawk--Mod--HideWindowsButton

Nesse repositório público está as modificações apresentadas no Canal @BitRizeBR(Youtube), Com ajuda do Claude.ai

Ocultar o Botão Iniciar (reforçado / hardened)
Oculta o botão Iniciar da barra de tarefas do Windows 11. O menu Iniciar ainda pode ser aberto com a tecla Win, Ctrl+Esc ou gestos em telas sensíveis ao toque. Apenas o Windows 11 é suportado.

Desativar o mod traz o botão de volta.

Créditos

Baseado no "Hide Start Button" de ptrkhh, que por sua vez é baseado no mod "Start button always on the left" (taskbar-start-button-position) de m417z. Este fork mantém o mesmo design e adiciona apenas código defensivo.

O que mudou em relação ao 1.0 original

    Checagens de ponteiro nulo (null checks) antes de desreferenciar ponteiros internos da barra de tarefas.

    Proteções contra exceções em C++ (exception guards) ao redor de cada chamada XAML e de cada callback de hook.

    A visibilidade do botão Iniciar só é gravada quando realmente muda, o que evita invalidações desnecessárias de layout no ciclo de renderização (arrange pass).

    O envio de mensagens entre threads (cross-thread SendMessage) agora tem um tempo limite (timeout) de 2 segundos, evitando que uma thread travada da barra de tarefas bloqueie o Explorer durante o carregamento/descarregamento do mod.

    Falhas são registradas no log do Windhawk (aba Log) em vez de ocorrerem de forma silenciosa.

Limitação conhecida

O mod ainda depende de rotinas internas não documentadas do Windows (símbolos da taskbar.dll e um índice fixo de tabela virtual da classe XAML UIElement). Uma atualização do Windows pode alterá-los. Se os símbolos não forem encontrados, o mod recusa o carregamento. Se apenas o índice da tabela virtual mudar, o Explorer pode apresentar comportamento anômalo; nesse caso, desative o mod e reinicie o Explorer.
