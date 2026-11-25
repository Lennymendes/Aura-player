// Aura — plug-in « Spectrogramme » (MIT).
// Cascade du spectre : le temps descend, le grave est à gauche, l'aigu à droite. Peu d'appels de dessin (la
// limite est de 60 ms par image) : 32 colonnes, une ligne tous les 3 images, 8 niveaux de couleur dessinés par lots.
const COLS = 32, ROWS = 40, EVERY = 3, LEVELS = 8;
const rows = [];                 // la plus récente d'abord
let acc = new Array(COLS).fill(0);
let frame = 0;

function mix(a, b, t) { return a + (b - a) * t; }
function hex(c) { const v = parseInt(c.slice(1), 16); return [(v >> 16) & 255, (v >> 8) & 255, v & 255]; }

function render(g, w, h, nx, slot) {
  const bands = (nx.audio && nx.audio.spectrum) || [];
  const per = Math.max(1, Math.floor((bands.length || COLS) / COLS));
  for (let c = 0; c < COLS; c++) {
    let m = 0;
    for (let k = 0; k < per; k++) m = Math.max(m, bands[c * per + k] || 0);
    if (m > acc[c]) acc[c] = m;                       // le maximum entre deux lignes : un transitoire ne se perd pas
  }
  if (++frame % EVERY === 0) {
    rows.unshift(acc);
    acc = new Array(COLS).fill(0);
    if (rows.length > ROWS) rows.pop();
  }

  // palette : fond -> accent 2 -> accent -> blanc ; le contraste sépare un couplet doux d'un drop
  const a1 = hex((nx.theme && nx.theme.accent) || "#00F5A0");
  const a2 = hex((nx.theme && nx.theme.accent2) || "#00D9F5");
  const palette = [];
  for (let l = 1; l <= LEVELS; l++) {
    const t = l / LEVELS;
    let r, gr, b;
    if (t < 0.45) { const u = t / 0.45; r = mix(10, a2[0], u); gr = mix(14, a2[1], u); b = mix(28, a2[2], u); }
    else if (t < 0.85) { const u = (t - 0.45) / 0.4; r = mix(a2[0], a1[0], u); gr = mix(a2[1], a1[1], u); b = mix(a2[2], a1[2], u); }
    else { const u = (t - 0.85) / 0.15; r = mix(a1[0], 255, u * 0.8); gr = mix(a1[1], 255, u * 0.8); b = mix(a1[2], 255, u * 0.8); }
    palette.push([r / 255, gr / 255, b / 255, 0.35 + 0.65 * t]);
  }

  const cw = w / COLS, rh = h / ROWS;
  const level = [];
  for (let r = 0; r < rows.length; r++) {
    const row = rows[r], fade = 1 - 0.4 * (r / ROWS);
    for (let c = 0; c < COLS; c++) {
      const v = Math.max(0, Math.min(1, ((row[c] || 0) - 0.2) / 0.75));    // 0,2 = silence de fond
      level.push(Math.min(LEVELS, Math.floor(Math.pow(v, 1.5) * fade * (LEVELS + 0.99))));
    }
  }
  for (let l = 1; l <= LEVELS; l++) {
    g.color(palette[l - 1]);
    for (let i = 0; i < level.length; i++) {
      if (level[i] === l) g.rect((i % COLS) * cw, Math.floor(i / COLS) * rh, cw + 0.5, rh + 0.5);
    }
  }
}
