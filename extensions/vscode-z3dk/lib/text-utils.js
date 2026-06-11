function formatBytes(size) {
  if (!Number.isFinite(size) || size <= 0) {
    return '';
  }
  if (size >= 1024 * 1024) {
    return `${(size / (1024 * 1024)).toFixed(2)} MB`;
  }
  if (size >= 1024) {
    return `${(size / 1024).toFixed(1)} KB`;
  }
  return `${size} B`;
}

function parseCsvLine(line) {
  const result = [];
  let current = '';
  let inQuotes = false;
  for (let i = 0; i < line.length; i += 1) {
    const char = line[i];
    if (char === '"') {
      if (inQuotes && line[i + 1] === '"') {
        current += '"';
        i += 1;
        continue;
      }
      inQuotes = !inQuotes;
      continue;
    }
    if (char === ',' && !inQuotes) {
      result.push(current);
      current = '';
      continue;
    }
    current += char;
  }
  result.push(current);
  return result;
}

function parseCsv(text) {
  const lines = text.split(/\r?\n/).filter(line => line.trim().length > 0);
  if (lines.length === 0) {
    return [];
  }
  const headers = parseCsvLine(lines[0]);
  const rows = [];
  for (const line of lines.slice(1)) {
    const values = parseCsvLine(line);
    const row = {};
    headers.forEach((header, index) => {
      row[header] = values[index] || '';
    });
    rows.push(row);
  }
  return rows;
}

function escapeHtml(value) {
  return String(value)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

function applyTemplate(value, replacements) {
  if (!value) {
    return '';
  }
  let output = value;
  for (const [key, replacement] of Object.entries(replacements)) {
    const safeValue = replacement || '';
    output = output.replace(new RegExp(`\\$\\{${key}\\}`, 'g'), safeValue);
  }
  return output;
}

function expandArgs(args, replacements) {
  if (!Array.isArray(args)) {
    return [];
  }
  return args.map(arg => applyTemplate(arg, replacements));
}

module.exports = {
  formatBytes,
  parseCsvLine,
  parseCsv,
  escapeHtml,
  applyTemplate,
  expandArgs
};
