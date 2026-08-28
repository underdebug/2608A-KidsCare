
vsce package --allow-missing-repository --skip-license
code --uninstall-extension local.mem
code --install-extension mem-0.0.1.vsix


: '

~/.vscode/extensions
/home/roots/.vscode/extensions/local.mem-0.0.1

/home/roots/.config/Code/User/keybindings.json

Ctrl + Shift + P
Reload Window
'


: << 'COMMENT'

1) test
Extension Development Host


2) install

node --version
npm --version

sudo npm install -g @vscode/vsce

cd f8comma
vsce package --allow-missing-repository --skip-license


Ctrl + Shit + P
Extensions: Install from VSIX...

Or:
code --install-extension f8comma-0.0.1.vsix

COMMENT


