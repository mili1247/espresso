configure_tutorial_target(
  TARGET
  tutorial_am
  DEPENDS
  active_matter.ipynb
  figures/friction.svg
  figures/pusher-puller.svg
  figures/enhanced.svg
  figures/geometry.svg
  figures/rectify.jpg
  figures/rectification.svg
  figures/flow_field.svg
  exercises/enhanced_diffusion.py
  exercises/flow_field.py
  exercises/rectification_geometry.py
  exercises/rectification_simulation.py
  solutions/enhanced_diffusion.py
  solutions/flow_field.py
  solutions/rectification_geometry.py
  solutions/rectification_simulation.py
  solutions/solutions.txt)

nb_export(TARGET tutorial_am SUFFIX "" FILE "active_matter.ipynb" HTML_RUN)
