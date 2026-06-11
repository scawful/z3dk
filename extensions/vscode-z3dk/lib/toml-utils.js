function stripTomlComment(line) {
  let inString = false;
  let escape = false;
  for (let i = 0; i < line.length; i += 1) {
    const c = line[i];
    if (escape) {
      escape = false;
      continue;
    }
    if (c === '\\') {
      escape = true;
      continue;
    }
    if (c === '"') {
      inString = !inString;
      continue;
    }
    if (!inString && c === '#') {
      return line.slice(0, i);
    }
  }
  return line;
}

function unescapeTomlString(value) {
  let output = '';
  let escape = false;
  for (const c of value) {
    if (escape) {
      switch (c) {
        case 'n':
          output += '\n';
          break;
        case 't':
          output += '\t';
          break;
        case 'r':
          output += '\r';
          break;
        case '\\':
        case '"':
          output += c;
          break;
        default:
          output += c;
          break;
      }
      escape = false;
    } else if (c === '\\') {
      escape = true;
    } else {
      output += c;
    }
  }
  return output;
}

function parseTomlString(value) {
  const trimmed = value.trim();
  if (!trimmed) {
    return '';
  }
  if (trimmed.startsWith('"') && trimmed.endsWith('"') && trimmed.length >= 2) {
    return unescapeTomlString(trimmed.slice(1, -1));
  }
  return trimmed;
}

function parseTomlStringArray(value) {
  const trimmed = value.trim();
  if (!trimmed) {
    return [];
  }
  if (!trimmed.startsWith('[') || !trimmed.endsWith(']')) {
    const single = parseTomlString(trimmed);
    return single ? [single] : [];
  }
  const inner = trimmed.slice(1, -1);
  const result = [];
  let current = '';
  let inString = false;
  let escape = false;
  for (const c of inner) {
    if (escape) {
      current += c;
      escape = false;
      continue;
    }
    if (c === '\\') {
      escape = true;
      continue;
    }
    if (c === '"') {
      inString = !inString;
      continue;
    }
    if (!inString && c === ',') {
      const token = current.trim();
      if (token) {
        const valueToken = parseTomlString(token);
        if (valueToken) {
          result.push(valueToken);
        }
      }
      current = '';
      continue;
    }
    current += c;
  }
  const token = current.trim();
  if (token) {
    const valueToken = parseTomlString(token);
    if (valueToken) {
      result.push(valueToken);
    }
  }
  return result;
}

module.exports = {
  stripTomlComment,
  parseTomlString,
  parseTomlStringArray
};
