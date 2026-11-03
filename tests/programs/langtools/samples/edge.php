<html><body>
<?php
$a = <<<EOT
  body $a
  more
  EOT;
$b = <<<'RAW'
raw text
RAW;
echo $a . $b; ?>
<p>tail</p>
<?php echo 1;
