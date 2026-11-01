const curve = document.getElementById('curve');

for (let i =0; i < 60; i++) {
    const bar = document.createElement('span');
    const height = 62 + 30 * Math.exp(-i / 6) + 3 * Math.sin(i * 1.7);
    bar.style.height = height + '%';
    if (i >= 54) bar.classList.add('scored');
    curve.appendChild(bar);
}

const barcode = document.getElementById('barcode');

[3,1,1,2,4,1,2,1,1,3,2,2,1,1,4,2,1,3,3,1,2,1,1,2,4,1,1,3,2,1,3,2,1,1,2,4,1,2,3,1].forEach((width) => {
    const bar = document.createElement('span');
    bar.style.flexGrow = width;
    barcode.appendChild(bar);
});