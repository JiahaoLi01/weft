// KaTeX's HTML output in #root: the formulas of CaseGinkgo's camera tuning panel, and Greek letters, scripts and
// relations, each in a row of its own, first inline (KaTeX's text style), then in display mode. KaTeX (third_party/katex)
// runs before this script; the page's stylesheet is katex.css and page.css.
(function () {
  const formulas = [
    String.raw`x(t) = \left(x_0 + (v_0 + \omega x_0)\,t\right) e^{-\omega t}`,
    String.raw`t_{50\%} \approx \frac{1.68}{\omega},\quad t_{90\%} \approx \frac{3.89}{\omega}`,
    String.raw`\theta(t) = \frac{J}{\omega_d} e^{-\zeta\omega t}\sin(\omega_d t),\qquad \omega_d = \omega\sqrt{1-\zeta^2},\quad f = \frac{\omega_d}{2\pi}`,
    String.raw`\theta_{\max} = \frac{J}{\omega}\exp\!\left(-\frac{\zeta}{\sqrt{1-\zeta^2}}\arctan\frac{\sqrt{1-\zeta^2}}{\zeta}\right)`,
    String.raw`\tan\frac{\theta_v}{2} = \frac{3}{4}\tan\frac{\theta_h}{2}`,
    String.raw`s \propto \frac{1}{d\,\tan(\theta/2)}`,
    String.raw`e_{ss} = \frac{2v\,(1-k)}{\omega}`,
    String.raw`P = P_0 + \sum_i w_i\,\Delta_i`,
    String.raw`n(t) = A\sum_{k=0}^{2} a_k \sin\left(2\pi f r_k t + \phi_k\right),\quad r = (1, \varphi, \varphi^2)`,
    String.raw`e = \operatorname{clamp}\left(\frac{s - s_0}{s_1 - s_0},\, 0,\, e_{\max}\right)`,
    String.raw`\alpha\beta\gamma\delta\epsilon\varepsilon\zeta\eta\theta\iota\kappa\lambda\mu\nu\xi\pi\rho\sigma\tau\upsilon\phi\varphi\chi\psi\omega\quad\Gamma\Delta\Theta\Lambda\Xi\Pi\Sigma\Upsilon\Phi\Psi\Omega`,
    String.raw`a \cdot b \le c \ge d,\quad x_i^2 + y^{n+1}_{j-1} \le z_{\max}`,
  ];
  const root = document.getElementById('root');
  for (const display of [false, true]) {
    for (const tex of formulas) {
      const row = document.createElement('div');
      row.className = display ? 'row display-row' : 'row';
      root.appendChild(row);
      katex.render(tex, row, {output: 'html', throwOnError: false, displayMode: display});
    }
  }
})();
