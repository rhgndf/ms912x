{ stdenv
, lib
, fetchFromGitHub
, kernel
}:

stdenv.mkDerivation rec {
  name = "ms912x";
  version = "1.0";

  src = fetchFromGitHub {
    owner = "rhgndf";
    repo = "ms912x";
    rev = "738aef1";
    sha256 = "16d2cjnsd9cf482jc7crigq1qsk0g7973dkzrck9b95sfclj7l8n";
  };

	setSourceRoot = ''
		export sourceRoot=$(pwd)/source
	'';

	nativeBuildInputs = kernel.moduleBuildDependencies;

	makeFlags = kernel.makeFlags ++ [
		"-C"
		"${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
		"M=$(sourceRoot)"
	];

  buildFlags = [ "modules" ]; 
	installFlags = [ "INSTALL_MOD_PATH=${placeholder "out"}" ];
  installTargets = [ "modules_install" ];


  meta = with lib; {
    description = "Drivers for MacroSilicon VGA Display Adapter";
    homepage = "https://github.com/rhgndf/ms912x";
    license = licenses.gpl2;
    platforms = platforms.linux;
  };
}
