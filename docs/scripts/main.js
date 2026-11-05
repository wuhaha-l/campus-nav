/* 交互层：滚动入场编排、算法对照台切换、简历文案一键复制 */
(function () {
  'use strict';

  var reduceMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

  /* 1. 滚动触发入场：只动 opacity / transform，reduced-motion 下全部直接可见 */
  var reveals = document.querySelectorAll('.reveal');
  if (reduceMotion || !('IntersectionObserver' in window)) {
    Array.prototype.forEach.call(reveals, function (el) { el.classList.add('in'); });
  } else {
    var observer = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) {
        if (entry.isIntersecting) {
          entry.target.classList.add('in');
          observer.unobserve(entry.target);
        }
      });
    }, { threshold: 0.12, rootMargin: '0px 0px -40px 0px' });
    Array.prototype.forEach.call(reveals, function (el) { observer.observe(el); });
  }

  /* 2. 算法对照台：切换的是「评价函数」，不是真实通行时间 */
  var states = {
    standard: {
      turns: '4 次',
      penalty: '0.0 s',
      total: '12.0 s',
      verdict: '标准 Dijkstra 只记录「到达某节点的最短时间」。<strong>两条路径评价耗时相同，先被松弛到的那条会被保留</strong>，结果可能是在网格里来回拐弯的锯齿路线。'
    },
    extended: {
      turns: '1 次',
      penalty: '0.5 s',
      total: '12.5 s',
      verdict: '状态扩展为 (来路, 节点) 后能识别转向：锯齿路线被罚到 14.0 s，直路 12.5 s 胜出。<strong>0.5 s 罚时远小于任何真实绕行代价，只改变等时路径的排序</strong>，不影响最优性；两条路实际通行时间仍是 12.0 s。'
    }
  };

  var diagram = document.querySelector('.diagram');
  var modeButtons = document.querySelectorAll('.seg button');
  var fields = {
    turns: document.getElementById('readout-turns'),
    penalty: document.getElementById('readout-penalty'),
    total: document.getElementById('readout-total'),
    verdict: document.getElementById('readout-verdict')
  };

  function setMode(mode) {
    var state = states[mode];
    if (!state) return;
    diagram.setAttribute('data-mode', mode);
    Array.prototype.forEach.call(modeButtons, function (btn) {
      btn.setAttribute('aria-pressed', String(btn.dataset.mode === mode));
    });
    fields.turns.textContent = state.turns;
    fields.penalty.textContent = state.penalty;
    fields.total.textContent = state.total;
    fields.verdict.innerHTML = state.verdict; // 硬编码常量，无外部输入
  }

  Array.prototype.forEach.call(modeButtons, function (btn) {
    btn.addEventListener('click', function () { setMode(btn.dataset.mode); });
  });

  /* 3. 复制简历文案：优先 Clipboard API，失败回退到 execCommand */
  var copyBtn = document.getElementById('copy-btn');
  var copyStatus = document.getElementById('copy-status');

  function fallbackCopy(text) {
    var area = document.createElement('textarea');
    area.value = text;
    area.setAttribute('readonly', '');
    area.style.position = 'fixed';
    area.style.opacity = '0';
    document.body.appendChild(area);
    area.select();
    var ok = document.execCommand('copy');
    document.body.removeChild(area);
    return ok;
  }

  function copyText(text) {
    if (navigator.clipboard && window.isSecureContext) {
      return navigator.clipboard.writeText(text);
    }
    return fallbackCopy(text) ? Promise.resolve() : Promise.reject(new Error('copy failed'));
  }

  copyBtn.addEventListener('click', function () {
    var text = document.getElementById('resume-text').textContent.trim() + '\n';
    copyText(text).then(function () {
      copyStatus.textContent = '已复制 / COPIED';
      copyBtn.textContent = '已复制';
    }).catch(function () {
      copyStatus.textContent = '复制失败，请手动选中文本';
    }).then(function () {
      setTimeout(function () {
        copyStatus.textContent = '';
        copyBtn.textContent = '复制这段文案';
      }, 2600);
    });
  });
})();
