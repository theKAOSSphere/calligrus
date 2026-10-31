// Console checks for the C++ TeX engine: every corpus formula renders (or is rejected cleanly), a
// token-soup fuzzer and the "abuse" inputs (deep nesting, huge sizes, invalid UTF-8) neither crash nor hang.
//
//   calligrus-texmath-test [corpus [ppem...]]      (default: 16 21 56; every formula, both styles)
//   calligrus-texmath-test fuzz [iterations] [seed]
//   calligrus-texmath-test abuse
//   options: --font <path>  --corpus <file>  (one formula per line, added to the built-in set)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "texmath.h"

namespace fs = std::filesystem;

// clang-format off
static const char* const kCorpus[] = {
    // --- mathpng.rs built-in set ---
    R"(x^2 + y^2 = z^2)",
    R"(\frac{a+b}{c+d} = \frac{1}{2})",
    R"(\sqrt{x^2+y^2} \quad \sqrt[3]{\frac{a}{b}})",
    R"(\sum_{i=1}^{n} i = \frac{n(n+1)}{2})",
    R"(\displaystyle \sum_{i=1}^{n} \frac{1}{i^2} = \frac{\pi^2}{6})",
    R"(\int_0^\infty e^{-x^2}\,dx = \frac{\sqrt{\pi}}{2})",
    R"(\displaystyle \int_{-\infty}^{\infty} e^{-x^2}\,dx = \sqrt{\pi})",
    R"(\left( \frac{a}{b} \right) \left[ \sum_{k=0}^{n} \binom{n}{k} x^k \right])",
    R"(\hat{x} \; \vec{v} \; \bar{z} \; \tilde{n} \; \widehat{ABC} \; \dot{q} \; \ddot{q})",
    R"(\mathbf{A}\mathbf{x} = \lambda \mathbf{x}, \quad \mathbb{R}^n, \; \mathcal{L}, \; \mathfrak{g})",
    R"(\begin{pmatrix} a & b \\ c & d \end{pmatrix} \begin{bmatrix} 1 & 0 \\ 0 & 1 \end{bmatrix})",
    R"(f(x) = \begin{cases} x^2 & \text{if } x \geq 0 \\ -x & \text{otherwise} \end{cases})",
    R"(\displaystyle \lim_{n \to \infty} \left(1 + \frac{1}{n}\right)^n = e)",
    R"(\overbrace{a+b+c}^{3} + \underbrace{x+y}_{2})",
    R"(\nabla \times \mathbf{E} = -\frac{\partial \mathbf{B}}{\partial t})",
    R"(\alpha \beta \gamma \Gamma \Delta \theta \vartheta \varphi \phi \epsilon \varepsilon \omega \Omega)",
    R"(\begin{aligned} a &= b + c \\ d &= e + f + g \end{aligned})",
    R"(\displaystyle \prod_{p} \frac{1}{1-p^{-s}} = \sum_{n=1}^\infty \frac{1}{n^s})",
    R"(e^{i\pi} + 1 = 0 \quad \forall x \in \mathbb{R}, \exists y : y > x)",
    R"(\frac{\partial^2 u}{\partial t^2} = c^2 \nabla^2 u)",
    R"(\det\begin{vmatrix} a & b \\ c & d \end{vmatrix} = ad - bc)",
    R"(\Big( \bigg( \Bigg( x \Bigg) \bigg) \Big))",
    R"(\left\{ \begin{array}{ll} 1 & x>0 \\ 0 & x \le 0 \end{array} \right.)",
    R"(\operatorname{softmax}(z)_i = \frac{e^{z_i}}{\sum_{j=1}^{K} e^{z_j}})",
    R"(\mathrm{d}x \; \unknowncmd \; a \leq b \neq c \approx d)",
    // --- demo.md ---
    R"(e^{i\pi} + 1 = 0)",
    R"(\sum_{k=1}^{n} k^2 = \frac{n(n+1)(2n+1)}{6})",
    R"(\alpha, \beta, \gamma)",
    R"(\hat{f}(\xi) = \int_{-\infty}^{\infty} f(x)\, e^{-2\pi i x \xi}\, dx)",
    R"(\int_{-\infty}^{\infty} e^{-x^2}\,dx = \sqrt{\pi})",
    R"(\sum_{n=1}^{\infty} \frac{1}{n^2} = \frac{\pi^2}{6} \qquad \prod_{p\ \text{prime}} \frac{1}{1-p^{-s}} = \sum_{n=1}^{\infty} \frac{1}{n^s})",
    "\\begin{aligned}\n\\nabla \\cdot \\mathbf{E} &= \\frac{\\rho}{\\varepsilon_0} \\\\\n\\nabla \\cdot \\mathbf{B} &= 0 \\\\\n\\nabla \\times \\mathbf{E} &= -\\frac{\\partial \\mathbf{B}}{\\partial t} \\\\\n\\nabla \\times \\mathbf{B} &= \\mu_0 \\mathbf{J} + \\mu_0 \\varepsilon_0 \\frac{\\partial \\mathbf{E}}{\\partial t}\n\\end{aligned}",
    R"(A = \begin{pmatrix} a_{11} & a_{12} & \cdots & a_{1n} \\ a_{21} & a_{22} & \cdots & a_{2n} \\ \vdots & \vdots & \ddots & \vdots \\ a_{m1} & a_{m2} & \cdots & a_{mn} \end{pmatrix}, \qquad |x| = \begin{cases} x & \text{if } x \ge 0 \\ -x & \text{if } x < 0 \end{cases})",
    R"(\left( \frac{\partial^2}{\partial x^2} + \frac{\partial^2}{\partial y^2} \right) u(x, y) = \lambda\, u(x, y) \tag{1})",
    R"(\lim_{n \to \infty} \left(1 + \frac{1}{n}\right)^{n} = e \qquad \sqrt[3]{\frac{x^3 + y^3}{2}} \qquad \overbrace{a + b + \cdots + z}^{26})",
    R"(\mathbb{R}^n)",
    R"(\sin x = \sum_{k=0}^\infty \frac{(-1)^k x^{2k+1}}{(2k+1)!})",
    R"(\Gamma(z) = \int_0^\infty t^{z-1} e^{-t}\,dt)",
    R"(\frac{1}{\sigma\sqrt{2\pi}} e^{-\frac{(x-\mu)^2}{2\sigma^2}})",
    // --- tests/smoke.rs ---
    R"(\frac{a+b}{c+d})",
    R"(\sqrt[3]{\frac{a}{b}})",
    R"(\int_0^\infty e^{-x^2}\,dx)",
    R"(\begin{pmatrix} a & b \\ c & d \end{pmatrix})",
    R"(f(x)=\begin{cases} x & x\ge 0 \\ -x & \text{otherwise}\end{cases})",
    R"(\left( \frac{a}{b} \right))",
    R"(\widehat{ABC} + \vec{v} + \overline{x})",
    R"(\overbrace{a+b}^{2} \xrightarrow{f} \boxed{x})",
    R"(\mathbb{R}^n \mathcal{L} \mathfrak{g} \alpha\beta\Gamma)",
    R"(E=mc^2 \tag{7})",
    R"(\frac{1}{)", R"(\left( x)", R"(\begin{pmatrix} a)", R"(x^)", R"(})", R"(\end{foo})",
    R"(a + \notacommand{x} + b)",
    R"(a\hspace{1000000em}b)",
    R"(\frac{1}{2})",
    // --- fractions & friends ---
    R"(\dfrac{1}{2} \tfrac{1}{2} \cfrac{1}{1+\cfrac{1}{2}})",
    R"(\binom{n}{k} \dbinom{n}{k} \tbinom{n}{k})",
    R"(a \over b)", R"(n \choose k)", R"(x \atop y)",
    R"({a+b \over c+d} + 1)",
    R"(\frac{\frac{\frac{1}{2}}{3}}{4})",
    R"(\frac{a^{2^{2^2}}}{b_{1_{1_1}}})",
    // --- radicals ---
    R"(\sqrt{x})", R"(\sqrt{\frac{a}{b}})", R"(\sqrt[n]{x^n+y^n})", R"(\sqrt{\sqrt{\sqrt{x}}})",
    R"(\sqrt{\frac{\frac{a}{b}}{\frac{c}{d}}})",
    R"(\sqrt{\begin{pmatrix}1\\2\\3\\4\\5\\6\end{pmatrix}})",
    R"(\surd 2)",
    // --- scripts ---
    R"(x_i^2 \; x^{2}_{i} \; x'' \; f'(x) \; a_{b_{c_{d}}} \; e^{x^{y^{z}}})",
    R"({}^{14}_{6}\mathrm{C} \; _a^b)",
    R"(P_{ij}^{kl} \; \Gamma_{\mu\nu}^\lambda \; V^{\prime})",
    R"(\int_a^b f \; \oint_C \; \iint_D \; \iiint_V \; \idotsint)",
    R"(\sum\limits_{i=0}^n \; \int\limits_0^1 \; \sum\nolimits_i \; \lim\nolimits_{x\to0})",
    R"(\textstyle\sum_{i=1}^n \displaystyle\sum_{i=1}^n \scriptstyle x^2 \scriptscriptstyle y^2)",
    // --- big operators & named operators ---
    R"(\prod \coprod \bigcup \bigcap \bigvee \bigwedge \bigoplus \bigotimes \bigodot \biguplus \bigsqcup)",
    R"(\int \iint \iiint \iiiint \oint \oiint \oiiint \intop)",
    R"(\sin\cos\tan\cot\sec\csc\sinh\cosh\tanh\coth\arcsin\arccos\arctan)",
    R"(\arg\deg\dim\exp\hom\ker\lg\ln\log\det\gcd\inf\lim\liminf\limsup\max\min\Pr\sup\injlim\projlim)",
    R"(a \bmod b \quad a \mod b \quad \Tr A \tr B \rank C \Re z \Im z)",
    R"(\max_{x \in X} f(x) \quad \operatorname*{argmax}_{\theta} L(\theta) \quad \DeclareMathOperator{foo})",
    // --- delimiters ---
    R"(\left( \left[ \left\{ \left\langle \left| \left\| x \right\| \right| \right\rangle \right\} \right] \right))",
    R"(\left\lfloor x \right\rfloor \left\lceil y \right\rceil \left. z \right| \left/ w \right\backslash)",
    R"(\left( \frac{\frac{a}{b}}{\frac{c}{d}} \right) \left[ \int_0^1 \right])",
    R"(\left( \begin{matrix} 1\\2\\3\\4\\5\\6\\7\\8\\9\\10 \end{matrix} \right))",
    R"(\left\{ x \middle| x > 0 \right\} \left\langle a \middle\vert b \right\rangle)",
    R"(\big( \Big[ \bigg\{ \Bigg\langle x \Bigg\rangle \bigg\} \Big] \big))",
    R"(\bigl( x \bigr) \Bigl[ y \Bigr] \biggl| z \biggr| \Biggl\| w \Biggr\| a \bigm| b)",
    R"(\big\lfloor \Big\lceil \bigg\uparrow \Bigg\Downarrow \big. \big/)",
    R"(\llbracket x \rrbracket \lgroup y \rgroup \lmoustache z \rmoustache)",
    R"(\abs{x} \norm{v} \ket{\psi} \bra{\phi} \braket{\phi}{\psi})",
    // --- matrices & environments ---
    R"(\begin{matrix} a & b \\ c & d \end{matrix})",
    R"(\begin{Bmatrix} 1 & 2 \\ 3 & 4 \end{Bmatrix} \begin{vmatrix} x \end{vmatrix} \begin{Vmatrix} y & z \end{Vmatrix})",
    R"(\begin{smallmatrix} a & b \\ c & d \end{smallmatrix})",
    R"(\begin{rcases} a & b \\ c & d \end{rcases} \begin{dcases} x \\ y \end{dcases})",
    R"(\begin{align} x &= 1 \\ y &= 2 & z &= 3 \end{align})",
    R"(\begin{alignedat}{2} a &= b & c &= d \end{alignedat})",
    R"(\begin{gathered} a = b \\ c = d + e \end{gathered} \begin{gather} x \end{gather})",
    R"(\begin{split} a &= b \\ &= c \end{split})",
    R"(\begin{array}{lcr} 1 & 22 & 333 \\ 4444 & 5 & 6 \end{array})",
    R"(\begin{array}{|c|c|} \hline a & b \\ \hline c & d \\ \hline \end{array})",
    R"(\begin{equation*} E = mc^2 \end{equation*} \begin{multline} a \end{multline})",
    R"(\begin{pmatrix} 1 & 2 \\[2pt] 3 & 4 \\ \end{pmatrix})",
    R"(\begin{matrix} \end{matrix})",
    R"(\begin{pmatrix} \frac{1}{2} & \sqrt{2} \\ \int_0^1 & \sum_i \end{pmatrix})",
    R"(\sum_{\substack{0 \le i \le m \\ 0 < j < n}} P(i, j))",
    R"(\sum_{\begin{subarray}{l} i \in I \\ j \in J \end{subarray}} a_{ij})",
    R"(a &= b \\ c &= d)",
    R"(x + y \\ z)",
    R"(\begin{tabular}{cc} a & b \end{tabular})",
    // --- accents & over/under ---
    R"(\hat a \check b \tilde c \acute d \grave e \dot f \ddot g \dddot h \breve i \bar j \mathring k \vec l)",
    R"(\widehat{xyz} \widetilde{abc} \overrightarrow{AB} \overleftarrow{CD} \widehat{abcdefghijklmn})",
    R"(\hat{\hat{x}} \bar{\bar{y}} \vec{\mathbf{v}} \tilde{A} \hat{H} \dot{\Omega})",
    R"(\overline{a+b} \underline{c+d} \overline{\overline{x}} \underline{\underline{y}})",
    R"(\overbrace{1+2+\cdots+100}^{5050} \underbrace{a+b+c+d+e+f+g}_{7})",
    R"(\overset{!}{=} \underset{x \to 0}{\lim} \stackrel{def}{=} \overset{\frown}{AB})",
    R"(\xrightarrow{f} \xleftarrow{g} \xrightarrow[below]{above} \xrightarrow{\text{a long label here}})",
    R"(\boxed{E=mc^2} \fbox{x} \boxed{\frac{a}{b}})",
    R"(\cancel{x} \bcancel{y} \xcancel{z} \cancel{\frac{a}{b}} \frac{\cancel{2}x}{\cancel{2}})",
    // --- fonts & text ---
    R"(\mathrm{ABC} \mathbf{abc} \mathit{xyz} \mathsf{Sans} \mathtt{mono} \mathcal{ABCDEFGHIJKLMNOPQRSTUVWXYZ})",
    R"(\mathbb{ABCDEFGHIJKLMNOPQRSTUVWXYZ} \mathbb{0123456789} \mathbb{abc} \mathfrak{ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz})",
    R"(\mathscr{L} \mathcal{abcdefghijklmnopqrstuvwxyz} \mathbf{0123456789} \mathsf{0123456789} \mathtt{0123456789})",
    R"(\boldsymbol{\alpha\beta\Gamma x} \bm{v} \mathbfit{A} \mathbf{\alpha\Omega} \mathit{\Gamma\Delta} \mathnormal{x})",
    R"(\text{plain text} \textbf{bold} \textit{italic} \textsf{sans} \texttt{mono} \textrm{roman} \mbox{box} \emph{em})",
    R"(\text{with } x \text{ inside and Ünïcødé})",
    R"(\rm roman \bf bold \it it \sf sans \tt mono \cal L \frak g \Bbb R)",
    R"(\mathbf{\hat{x}} \mathrm{d}x \mathrm{e}^{x} \mathit{diff})",
    // --- colors & misc commands ---
    R"(\color{red} x + \textcolor{blue}{y} + \mathcolor{green}{z} + \colorbox{yellow}{w})",
    R"(\phantom{xyz}a \hphantom{xyz}b \vphantom{\int}c \mathstrut d)",
    R"(\mathop{a} b \mathbin{c} d \mathrel{e} f \mathopen{g} \mathclose{h} \mathpunct{i} \mathinner{j} \mathord{+})",
    R"(\not= \not< \not> \not\in \not\subset \not\equiv \not\approx \not\leq \not\to \not\exists \not x)",
    R"(\label{eq} \nonumber \notag \tag*{A} x)",
    R"(\ensuremath{x} \dd x \vb{F} \hline \cline{1-2})",
    R"(\tag{1.2.3} a = b)",
    // --- spacing ---
    R"(a\,b\:c\;d\!e\ f~g\quad h\qquad i\enspace j)",
    R"(a\thinspace b\medspace c\thickspace d\negthinspace e\negmedspace f\negthickspace g)",
    R"(a\hspace{1em}b\hspace{2ex}c\mkern18mu d\kern3pt e\mskip-6mu f\hskip1cm g\hspace*{10px}h)",
    R"(a\kern 1em b\kern-0.5em c\hspace{1in}d\hspace{5mm}e\hspace{0.3}f\hspace{bogus}g)",
    R"(\hspace{1e3em}x)", R"(\hspace{nanem}x)", R"(a\hspace{-1e9em}b)",
    // --- all symbol classes ---
    R"(\pm\mp\times\div\cdot\ast\star\circ\bullet\oplus\ominus\otimes\oslash\odot\cup\cap\uplus\sqcup\sqcap)",
    R"(a\vee b\lor c\wedge d\land e\setminus f\smallsetminus g\wr h\dagger i\ddagger j\diamond k)",
    R"(\triangleleft\triangleright\bigtriangleup\bigtriangledown\boxplus\boxminus\boxtimes\ltimes\rtimes\amalg)",
    R"(a\leq b\le c\geq d\ge e\neq f\ne g\equiv h\approx i\sim j\simeq k\cong l\propto m\ll n\gg o)",
    R"(\subset\supset\subseteq\supseteq\subsetneq\supsetneq\nsubseteq\in\ni\owns\notin\sqsubset\sqsupset\sqsubseteq\sqsupseteq)",
    R"(\perp\parallel\nparallel\mid\nmid\vdash\dashv\models\asymp\doteq\prec\succ\preceq\succeq\lesssim\gtrsim\triangleq\coloneqq\bowtie\smile\frown)",
    R"(\to\rightarrow\leftarrow\gets\leftrightarrow\Rightarrow\Leftarrow\Leftrightarrow\implies\Longrightarrow\impliedby\Longleftarrow\iff\Longleftrightarrow)",
    R"(\mapsto\longmapsto\longrightarrow\longleftarrow\longleftrightarrow\uparrow\downarrow\updownarrow\Uparrow\Downarrow\Updownarrow)",
    R"(\nearrow\searrow\swarrow\nwarrow\hookrightarrow\hookleftarrow\rightharpoonup\rightharpoondown\leftharpoonup\leftharpoondown\rightleftharpoons\leadsto\rightsquigarrow\twoheadrightarrow\twoheadleftarrow\nrightarrow\nleftarrow)",
    R"(\infty\partial\nabla\forall\exists\nexists\emptyset\varnothing\neg\lnot\top\bot\angle\measuredangle\triangle\ell\hbar\hslash\Re\Im\aleph\beth\gimel\wp\imath\jmath)",
    R"(\prime\backprime\surd\Box\square\blacksquare\lozenge\spadesuit\heartsuit\diamondsuit\clubsuit\flat\natural\sharp\checkmark\degree\complement\eth\mho\S\P\copyright\pounds\yen\euro)",
    R"(\ldots\dots\dotsc\dotso\mathellipsis\cdots\dotsb\dotsm\dotsi\cdotp\vdots\ddots\iddots\colon\ldotp)",
    R"(\textbackslash\backslash\lbrace\rbrace\langle\rangle\lfloor\rfloor\lceil\rceil\lvert\vert\rvert\lVert\rVert\Vert\lbrack\rbrack)",
    R"(\{ \} \| \$ \% \& \# \_ \, \backslash)",
    R"(\alpha\beta\gamma\delta\epsilon\varepsilon\zeta\eta\theta\vartheta\iota\kappa\varkappa\lambda\mu\nu\xi\omicron\pi\varpi\rho\varrho\sigma\varsigma\tau\upsilon\phi\varphi\chi\psi\omega)",
    R"(\Gamma\Delta\Theta\Lambda\Xi\Pi\Sigma\Upsilon\Phi\Psi\Omega \mathit{\Gamma\Omega})",
    R"(a+b-c*d=e<f>g:h(i)[j]!k?l,m;n`o)",
    R"(×÷±∓·⋅∘≤≥≠≈≡∈∉⊂⊆→←⇒⇔∼ αβγ ∑∫ é 𝓐)",
    R"(-a + +b \cdot -c ( -d ) = -e, -f)",
    R"(a + \quad + b \; = \, - c)",
    // --- malformed / edge ---
    "", "   ", "\\", "{", "{{}", "{}}", "x_", "x^{", "\\frac", "\\frac{a}", "\\sqrt[", "\\sqrt[3",
    "\\left(", "\\left( x \\right", "\\right)", "\\middle|", "\\begin{matrix}", "\\begin{foo}x\\end{foo}",
    "\\begin{matrix} a \\end{pmatrix}", "a & b", "a \\\\ b \\end{x} c", "\\end", "&", "$x$", "x^^2",
    "x__2", "\\text{unclosed", "\\hspace", "\\big", "\\left\\notadelim x \\right.", "\\overset{a}",
    "\\substack{a", "\\substack{a & b}", "\\xrightarrow[", "\\color", "\\begin{array}", "\\begin{alignedat}",
    "\\tag{", "\\not", "\\not\\frac{1}{2}", "\\operatorname", "\\limits", "^2", "_2", "'", "''x",
    "\\begin{cases}\\end{cases}", "\\\\\\\\", "a\\\\", "\\left.\\right.", "\\sqrt[]{}", "\\frac{}{}",
    "\\binom{}{}", "\\overbrace{}^{}", "\\boxed{}", "\\cancel{}", "\\xrightarrow{}", "\\widehat{}",
    "\\sum_{}^{}", "\\int\\limits_{}^{}", "\\mathbb{}", "\\text{}", "{}", "\\phantom{}", "\\begin{matrix}&\\end{matrix}",
};
// clang-format on

static fs::path exe_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
#else
    return fs::read_symlink("/proc/self/exe").parent_path();
#endif
}

static std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }
    return out.size() > 110 ? out.substr(0, 110) + "..." : out;
}

static std::vector<std::string> load_corpus(const std::string& extra) {
    std::vector<std::string> v(std::begin(kCorpus), std::end(kCorpus));
    if (!extra.empty()) {
        std::ifstream in(extra);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) v.push_back(line);
        }
    }
    return v;
}

// Small xorshift RNG so runs are reproducible (same as mathfuzz.rs).
struct Rng {
    uint64_t s;
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    size_t below(size_t n) { return size_t(next() % n); }
};

static const char* const kTokens[] = {
    "x", "y", "1", "2", "+", "-", "=", "(", ")", "[", "]", "{", "}", "^", "_", "&", "\\\\", " ", ",", "'", "|",
    "\\frac", "\\sqrt", "\\sum", "\\int", "\\prod", "\\lim", "\\left", "\\right", "\\left(", "\\right)", "\\left.", "\\right|",
    "\\begin{pmatrix}", "\\end{pmatrix}", "\\begin{cases}", "\\end{cases}", "\\begin{aligned}", "\\end{aligned}",
    "\\begin{array}{cc}", "\\end{array}", "\\text{ab c}", "\\mathbb{R}", "\\mathbf", "\\hat", "\\widehat", "\\vec", "\\bar",
    "\\overline", "\\underline", "\\overbrace", "\\underbrace", "\\overset", "\\underset", "\\binom", "\\xrightarrow",
    "\\boxed", "\\cancel", "\\alpha", "\\Gamma", "\\infty", "\\partial", "\\nabla", "\\cdots", "\\ldots", "\\quad", "\\,",
    "\\limits", "\\nolimits", "\\displaystyle", "\\big", "\\Bigg", "\\bigl(", "\\bigr)", "\\operatorname{foo}", "\\tag{1}",
    "\\\\[2pt]", "\\substack", "\\over", "\\choose", "\\not", "\\mathrm", "\\phantom", "\\ket", "\\abs", "\\unknown",
    "\\", "$", "%", "~", "\\begin{matrix}", "\\end{matrix}", "\\end{foo}", "\\begin{bar}", "\\left[", "\\right]", "\\middle|",
    "\\sqrt[3]", "\\xleftarrow[a]", "\\mathcal{L}", "\\mathfrak{g}", "\\sum_{i=1}^{n}", "\\int_0^\\infty", "é", "∑", "→", "𝓐",
    "\\bcancel", "\\xcancel", "\\hspace{2em}", "\\kern-1em", "\\middle\\|", "\\big\\{", "\\dfrac", "\\color{red}",
};

static std::string fuzz_formula(Rng& rng) {
    const size_t n = 1 + rng.below(14);
    std::vector<const char*> parts;
    for (size_t i = 0; i < n; i++) parts.push_back(kTokens[rng.below(std::size(kTokens))]);
    const char* sep = rng.below(2) == 0 ? "" : " ";
    std::string s;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) s += sep;
        s += parts[i];
    }
    return s;
}


int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::vector<std::string> args(argv + 1, argv + argc);
    fs::path font = exe_dir() / "assets" / "fonts" / "latinmodern-math.otf";
    std::string corpus_file;
    std::vector<std::string> pos;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "--font" && i + 1 < args.size()) {
            font = args[++i];
        } else if (args[i] == "--corpus" && i + 1 < args.size()) {
            corpus_file = args[++i];
        } else {
            pos.push_back(args[i]);
        }
    }
    std::string err;
    if (!texmath::init_font(font, &err)) {
        std::printf("font: %s\n", err.c_str());
        return 2;
    }
    const std::string mode = pos.empty() ? "corpus" : pos[0];

    if (mode == "corpus") {
        std::vector<float> ppems;
        for (size_t i = 1; i < pos.size(); i++) ppems.push_back(std::stof(pos[i]));
        if (ppems.empty()) ppems = {16, 21, 56};
        const auto corpus = load_corpus(corpus_file);
        int ok = 0, rejected = 0, empty = 0;
        for (float p : ppems)
            for (const std::string& t : corpus)
                for (bool display : {false, true}) {
                    auto r = texmath::render(t, display, p);
                    if (auto* eq = std::get_if<texmath::Equation>(&r)) {
                        ok++;
                        if (eq->main.width == 0 || eq->main.height == 0) empty++;
                    } else {
                        rejected++;
                    }
                }
        std::printf("%zu formulas x %zu ppem x 2 styles: %d rendered (%d empty bitmaps), %d rejected\n", corpus.size(),
                    ppems.size(), ok, empty, rejected);
        return empty ? 1 : 0;
    }
    if (mode == "fuzz") {
        const int iters = pos.size() > 1 ? std::stoi(pos[1]) : 20000;
        Rng rng{pos.size() > 2 ? std::stoull(pos[2]) : 0x9E3779B97F4A7C15ull};
        int ok = 0, rejected = 0;
        const auto t0 = std::chrono::steady_clock::now();
        double worst = 0;
        std::string worst_src;
        for (int i = 0; i < iters; i++) {
            const std::string src = fuzz_formula(rng);
            const bool display = rng.below(2) == 0;
            const auto a = std::chrono::steady_clock::now();
            auto r = texmath::render(src, display, 24.0f);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
            if (ms > worst) worst = ms, worst_src = src;
            (std::holds_alternative<texmath::Equation>(r) ? ok : rejected)++;
        }
        const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("%d formulas: %d rendered, %d rejected cleanly, 0 crashed (%.2f s, slowest %.1f ms: `%s`)\n", iters, ok,
                    rejected, total, worst, escape(worst_src).c_str());
        return 0;
    }
    if (mode == "abuse") {
        auto rep = [](const std::string& s, int n) {
            std::string o;
            for (int i = 0; i < n; i++) o += s;
            return o;
        };
        std::vector<std::pair<const char*, std::string>> cases = {
            {"deep braces", rep("{", 5000) + "x" + rep("}", 5000)},
            {"deep fracs", rep("\\frac{1}{", 2000) + "x" + rep("}", 2000)},
            {"deep scripts", "x" + rep("^{x", 3000) + rep("}", 3000)},
            {"deep left/right", rep("\\left(", 1000) + "x" + rep("\\right)", 1000)},
            {"deep sqrt", rep("\\sqrt{", 3000) + "x" + rep("}", 3000)},
            {"deep opt arg", rep("\\sqrt[", 3000) + "x" + rep("]", 3000) + "{y}"},
            {"deep tags", rep("\\tag{", 3000) + "x" + rep("}", 3000)},
            {"huge space", "a\\hspace{1000000em}b"},
            {"negative huge", "a\\hspace{-1e9em}b"},
            {"nan-ish", "\\hspace{nanem}x"},
            {"empty", ""},
            {"only space", "   "},
            {"only backslash", "\\"},
            {"unclosed begin", "\\begin{pmatrix} a & b"},
            {"long row", rep("x+", 20000) + "y"},
            {"wide matrix", "\\begin{matrix}" + rep("a&", 3000) + "b\\end{matrix}"},
            {"tall stack", "\\begin{matrix}" + rep("a\\\\", 3000) + "b\\end{matrix}"},
            {"tall delims", "\\left(\\begin{matrix}" + rep("a\\\\", 300) + "a\\end{matrix}\\right\\}"},
            {"tiled 9600px", rep("x\\quad ", 380) + "\\cancel{y}\\sqrt{\\frac{a}{b}}"},
            {"tiled 12000px", "\\left(" + rep("\\frac{\\int_0^1 f}{\\sum_k \\hat{y}}\\quad ", 160) + "\\right)"},
            {"wide 9000px", "\\overbrace{" + rep("a+", 60) + "}^{x}" + rep("\\quad", 140) + "\\cancel{x}"},
            {"long text", "\\text{" + rep("abc ", 5000) + "}"},
            {"invalid utf8", std::string("x\xff\xfe y")},
        };
        for (auto& [name, src] : cases) {
            const auto t = std::chrono::steady_clock::now();
            auto r = texmath::render(src, true, 24.0f);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
            if (auto* eq = std::get_if<texmath::Equation>(&r)) {
                std::printf("%-16s %8.1f ms  rendered %ux%u\n", name, ms, eq->main.width, eq->main.height);
            } else {
                std::printf("%-16s %8.1f ms  error: %s\n", name, ms, std::get<std::string>(r).c_str());
            }
        }
        return 0;
    }
    if (mode == "capture") {  // spec 33: glyphs with a character leave the bitmap and come back as MathGlyphs (as texmath/mod.rs's test)
        int failures = 0;
        auto check = [&](bool ok, const char* what) {
            if (!ok) std::printf("FAIL %s\n", what), failures++;
        };
        auto main_of = [](const std::string& tex, bool display, bool capture) {
            auto r = texmath::render(tex, display, 32.0f, capture);
            auto* eq = std::get_if<texmath::Equation>(&r);
            return eq ? eq->main : texmath::Rendered{};
        };
        auto inked = [](const texmath::Rendered& r) {
            for (size_t i = 3; i < r.rgba.size(); i += 4)
                if (r.rgba[i]) return true;
            return false;
        };
        auto text = [](const texmath::Rendered& r) {
            std::string t;
            for (const auto& g : r.glyphs) t += g.text;
            return t;
        };
        const auto plain = main_of("x+y", false, false), cap = main_of("x+y", false, true);
        check(plain.glyphs.empty() && inked(plain), "plain render has ink and no glyphs");
        check(text(cap) == "x+y", "x+y glyph text (math italic folded to plain letters)");
        check(!inked(cap), "glyphs are left out of the bitmap");
        check(cap.width == plain.width && cap.height == plain.height && cap.origin_x == plain.origin_x && cap.origin_y == plain.origin_y, "same bitmap size and origin");
        bool order = cap.glyphs.size() == 3, level = true;
        for (size_t i = 1; i < cap.glyphs.size(); i++) order &= cap.glyphs[i - 1].x < cap.glyphs[i].x;
        for (const auto& g : cap.glyphs) level &= g.size == 32.0f && g.y == cap.origin_y;
        check(order && level, "glyphs left to right on the baseline at 32 px per em");
        const auto bar = main_of("\\frac{x}{y}", false, true);
        check(text(bar) == "xy" && inked(bar), "a fraction's bar stays in the bitmap, the letters go");
        const auto big = main_of("\\left(\\frac{a}{b}\\right)", true, true);
        check(inked(big), "big delimiters stay in the bitmap");
        std::printf(failures ? "%d capture checks failed\n" : "texmath capture: all checks passed\n", failures);
        return failures ? 1 : 0;
    }
    std::printf("usage: calligrus-texmath-test [corpus [ppem...] | fuzz [n] [seed] | abuse | capture] [--font f] [--corpus f]\n");
    return 2;
}
